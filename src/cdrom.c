/* cdrom.c - the resident CD-ROM device driver of the LEADOUT client
 *
 * Answers the MSCDEX device driver requests: IOCTL input and output, read
 * long, play, stop and resume. The disc info comes from the server's OFFER.
 * Audio status is asked from the server, at most once per timer tick.
 * start.asm calls dev_request() with DS = SS = DGROUP and req_ptr set.
 *
 * All resident variables are initialized, so they stay in _DATA.
 */
#include "leadout.h"

void far *req_ptr = 0;                  /* the request header, set by start.asm */
struct info info = {0};

/* CD audio state, as the server last reported it */
static u8  au_state = 0;                /* 0 stopped, 1 playing, 2 paused */
static u8  au_fresh = 0;
static u16 au_tick = 0;
static u32 au_pos = 0, au_start = 0, au_end = 0;
static u8  vol_l = 255, vol_r = 255;

/* Disc change */
static u8  media_stale = 0;             /* 1: check the disc at the next request */

/* Read-ahead. Games and video players read in pieces of one or two sectors,
 * and each piece would cost a round trip. So a small read fetches a whole
 * block into this buffer, and the next reads come from it in microseconds,
 * as from a drive's own buffer. The buffer sits after the resident code,
 * where the start-up code was (init.c keeps that memory). */
u16 cache_sectors = BLOCK_MAX;          /* /C:n, 0 = off, at most BLOCK_MAX */
u16 cache_off = 0;                      /* offset of the buffer in this segment */
static u32 c_lba = 0;                   /* first sector in the buffer */
static u16 c_n = 0;                     /* sectors in the buffer, 0 = empty */
static u8  c_disc = 0;                  /* the disc they came from */
static u16 c_tick = 0;                  /* when they came: a block is good for CACHE_TICKS */
static u16 media_tick = 0;
static u8  media_checked = 0;
static u8  media_report = 0;            /* 1: the disc changed, tell DOS at the next IOCTL 9 */

/* ------------------------------------------------------------------------
 * CD audio
 * ---------------------------------------------------------------------- */
static u32 redbook_to_lba(u8 frame, u8 sec, u8 min)
{
    return mul16(min, 4500) + mul16(sec, 75) + frame - 150;
}

/* LBA -> minute, second, frame (with or without the 150-frame offset) */
static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f)
{
    u16 r;
    u16 secs = div32(lba, 75, &r);
    *f = (u8)r;
    *m = (u8)(secs / 60);
    *s = (u8)(secs % 60);
}

/* LBA -> Red Book dword: frame, second, minute, 0 */
static u32 lba_to_redbook(u32 lba)
{
    u8 m, s, f;
    lba_to_msf(lba + 150, &m, &s, &f);
    return (u32)f | ((u32)s << 8) | ((u32)m << 16);
}

/* Ask the server for the audio status, at most once per timer tick. */
static void audio_refresh(void)
{
    if (au_fresh && (u16)(ticks() - au_tick) < 1)
        return;
    if (ncd_request(T_ASTATUS, 0) == ST_OK) {
        au_state = rep_buf[0];
        au_pos = *(u32 *)(rep_buf + 2);
        au_start = *(u32 *)(rep_buf + 6);
        au_end = *(u32 *)(rep_buf + 10);
    } else {
        au_state = 0;
    }
    if (au_state == 0)
        hb_active = 0;
    au_tick = ticks();
    au_fresh = 1;
}

static u16 play_audio(u8 far *rq)
{
    u32 start = FL(rq + 0x0E);
    u32 count = FL(rq + 0x12);
    if (rq[0x0D] != 0)
        start = redbook_to_lba(rq[0x0E], rq[0x0F], rq[0x10]);
    *(u32 *)(txbuf + NCD_BODY) = start;
    *(u32 *)(txbuf + NCD_BODY + 4) = count;
    au_fresh = 0;
    if (ncd_request(T_PLAY, 8) != ST_OK)
        return DEV_ERR_GENERAL;
    au_state = count ? 1 : 0;
    hb_active = au_state;
    hb_ticks = 0;
    au_start = start;
    au_end = start + count;
    return DEV_DONE;
}

static u16 stop_audio(void)
{
    au_fresh = 0;
    ncd_request(T_STOP, 0);
    au_state = (au_state == 1) ? 2 : 0;
    hb_active = au_state ? 1 : 0;       /* paused still counts as active */
    return DEV_DONE;
}

static u16 resume_audio(void)
{
    au_fresh = 0;
    if (ncd_request(T_RESUME, 0) != ST_OK)
        return DEV_ERR_GENERAL;
    au_state = 1;
    hb_active = 1;
    return DEV_DONE;
}

/* Q-channel: the track, the time in the track, and the time on the disc. */
static void q_channel(u8 far *cb)
{
    u8 *e, *best = 0;
    u32 best_lba = 0, t;
    u16 i;
    u8 m, s, f;

    audio_refresh();
    for (i = 0, e = info.toc; i < info.ntracks; i++, e += 6) {
        t = redbook_to_lba(e[2], e[3], e[4]);
        if (t <= au_pos && (!best || t >= best_lba)) {
            best = e;
            best_lba = t;
        }
    }
    if (!best) {
        fzero(cb + 1, 10);
        return;
    }
    cb[1] = best[1];                            /* control and ADR */
    cb[2] = (u8)(((best[0] / 10) << 4) | (best[0] % 10));  /* track, BCD */
    cb[3] = 1;                                  /* index */
    lba_to_msf(au_pos - best_lba, &m, &s, &f);  /* time in the track */
    cb[4] = m; cb[5] = s; cb[6] = f;
    cb[7] = 0;
    lba_to_msf(au_pos + 150, &m, &s, &f);       /* time on the disc */
    cb[8] = m; cb[9] = s; cb[10] = f;
}

/* ------------------------------------------------------------------------
 * Disc change
 * ---------------------------------------------------------------------- */

/* Returns 1 if the disc changed, 0 if not, -1 if the server did not answer.
 * Asks the server at most once per timer tick. On a change, it gets the
 * new disc info. */
static int media_check(void)
{
    if (!media_stale && media_checked && (u16)(ticks() - media_tick) < 1)
        return 0;
    media_tick = ticks();
    media_checked = 1;
    if (ncd_request(T_MEDIA, 0) != ST_OK)
        return -1;
    if (!media_stale && rep_buf[0] == info.disc_id)
        return 0;
    media_stale = 0;
    au_state = 0;
    hb_active = 0;
    au_fresh = 0;
    if (discover(0))
        return -1;
    media_report = 1;
    return 1;
}

/* ------------------------------------------------------------------------
 * Device driver requests
 * ---------------------------------------------------------------------- */
static u16 ioctl_in(u8 far *rq)
{
    u8 far *cb = *(u8 far * far *)(rq + 0x0E);
    u8 *e;
    u16 i;

    if (media_stale && (cb[0] == 6 || cb[0] == 8 || cb[0] == 10 || cb[0] == 11))
        media_check();                  /* a SELECT from LEADOUT /DISC: get the new disc first */

    switch (cb[0]) {
    case 0:                             /* device header address */
        *(void far * far *)(cb + 1) = (void far *)devhdr;
        break;
    case 1:                             /* head location */
        FL(cb + 2) = 0;
        break;
    case 4:                             /* audio channel info */
        cb[1] = 0; cb[2] = vol_l;
        cb[3] = 1; cb[4] = vol_r;
        cb[5] = 2; cb[6] = 0;
        cb[7] = 3; cb[8] = 0;
        break;
    case 6:                             /* device status */
        /* door unlocked, plays audio, audio channel control,
           HSG and Red Book addresses */
        FL(cb + 1) = info.ntracks ? 0x00000312UL : 0x00000B12UL;  /* bit 11: no disc */
        break;
    case 7:                             /* sector size */
        FW(cb + 2) = 2048;
        break;
    case 8:                             /* volume size */
        FL(cb + 1) = info.sectors;
        break;
    case 9:                             /* media changed */
        i = media_check();
        if (media_report) {
            media_report = 0;
            i = 1;
        }
        cb[1] = (i == 1) ? 0xFF : (i == 0 ? 1 : 0);   /* changed / not / unknown */
        break;
    case 10:                            /* audio disk info */
        cb[1] = info.first;
        cb[2] = info.last;
        fcopy(cb + 3, info.leadout, 4);
        break;
    case 11:                            /* audio track info */
        for (i = 0, e = info.toc; i < info.ntracks; i++, e += 6) {
            if (e[0] == cb[1]) {
                fcopy(cb + 2, e + 2, 4);
                cb[6] = e[1];
                return DEV_DONE;
            }
        }
        return DEV_ERR_NOT_FOUND;
    case 12:                            /* Q channel */
        q_channel(cb);
        break;
    case 15:                            /* audio status */
        audio_refresh();
        FW(cb + 1) = (au_state == 2) ? 1 : 0;
        FL(cb + 3) = lba_to_redbook(au_start);
        FL(cb + 7) = lba_to_redbook(au_end);
        break;
    }
    return DEV_DONE;
}

static u16 ioctl_out(u8 far *rq)
{
    u8 far *cb = *(u8 far * far *)(rq + 0x0E);
    if (cb[0] == 3) {                   /* audio channel control */
        vol_l = cb[2];
        vol_r = cb[4];
        txbuf[NCD_BODY] = vol_l;
        txbuf[NCD_BODY + 1] = vol_r;
        ncd_request(T_VOLUME, 2);
    }
    return DEV_DONE;
}

static u16 read_long(u8 far *rq)
{
    u8 far *buf = *(u8 far * far *)(rq + 0x0E);
    u16 cnt = FW(rq + 0x12);
    u32 lba = FL(rq + 0x14);

    if (rq[0x18] != 0)                  /* cooked mode only */
        return DEV_ERR_UNKNOWN_CMD;
    if (media_stale || !info.ntracks) {
        int m = media_check();
        if (m == 1)
            return DEV_ERR_DISC_CHANGE;
        if (!info.ntracks)
            return DEV_ERR_NOT_READY;
    }
    if (rq[0x0D] != 0) {                /* Red Book: frame, second, minute */
        lba = mul16(rq[0x16], 4500) + mul16(rq[0x15], 75) + rq[0x14] - 150;
    }
    while (cnt) {
        u16 n = cnt < block_max ? cnt : block_max;
        buf = MK_FP(FP_SEG(buf) + (FP_OFF(buf) >> 4), FP_OFF(buf) & 15);
        if (cache_sectors) {
            if (c_n && c_disc == info.disc_id && lba >= c_lba && lba < c_lba + c_n &&
                (u16)(ticks() - c_tick) < CACHE_TICKS) {
                u16 have = (u16)(c_lba + c_n - lba);
                if (n > have)
                    n = have;
                fcopy(buf, (u8 *)(cache_off + (u16)(lba - c_lba) * 2048u), n * 2048u);
                goto next;
            }
            if (cnt < cache_sectors) {  /* a small read: fetch a whole block */
                u8 far *cb = MK_FP(FP_SEG((void far *)&c_n), cache_off);
                if (net_read(lba, (u8)cache_sectors, cb) == 0) {
                    c_lba = lba;
                    c_n = cache_sectors;
                    c_disc = info.disc_id;
                    c_tick = ticks();
                    continue;           /* served from the buffer above */
                }
                /* near the end of the track the block does not fit: read
                   exactly what was asked, below */
            }
        }
        switch (net_read(lba, (u8)n, buf)) {
        case 0:
            break;
        case ST_CHANGED:                /* the disc changed on the server */
            media_stale = 1;
            return DEV_ERR_DISC_CHANGE;
        case ST_NODISC:
            media_stale = 1;
            return DEV_ERR_NOT_READY;
        default:
            return DEV_ERR_READ;
        }
next:
        cnt -= n;
        lba += n;
        buf += n * 2048u;
    }
    return DEV_DONE;
}

/* Vendor request from a second copy of LEADOUT (LEADOUT /DISC).
 * rq+0x0D subfunction, rq+0x0E far buffer, rq+0x12 buffer size,
 * rq+0x14 argument (LIST: first index, SELECT: index). */
static u16 vendor_request(u8 far *rq)
{
    int r;
    W(txbuf + NCD_BODY) = FW(rq + 0x14);
    if (rq[0x0D] == RQ_LIST) {
        rep_dst = *(u8 far * far *)(rq + 0x0E);
        rep_max = FW(rq + 0x12);
        r = ncd_request(T_LIST, 2);
        rep_dst = 0;
        FW(rq + 0x16) = rep_len;
    } else if (rq[0x0D] == RQ_SELECT) {
        r = ncd_request(T_SELECT, 2);
        media_stale = 1;
        if (r == ST_OK)
            media_check();              /* load the new track list now; IOCTL 9 reports the change */
    } else {
        return DEV_ERR_UNKNOWN_CMD;
    }
    if (r < 0)
        return DEV_ERR_NOT_READY;       /* no reply */
    return r ? DEV_ERR_GENERAL : DEV_DONE;
}

void dev_request(void)
{
    u8 far *rq = req_ptr;
    u16 st;

    switch (rq[2]) {
    case CMD_IOCTL_IN:  st = ioctl_in(rq); break;
    case CMD_IOCTL_OUT: st = ioctl_out(rq); break;
    case CMD_READ_LONG: st = read_long(rq); break;
    case CMD_PLAY:      st = play_audio(rq); break;
    case CMD_STOP:      st = stop_audio(); break;
    case CMD_RESUME:    st = resume_audio(); break;
    case RQ_LEADOUT:    st = vendor_request(rq); break;
    case CMD_IN_FLUSH:
    case CMD_OUT_FLUSH:
    case CMD_OPEN:
    case CMD_CLOSE:
    case CMD_PREFETCH:
    case CMD_SEEK:
        st = DEV_DONE;
        break;
    default:
        st = DEV_ERR_UNKNOWN_CMD;
    }
    /* Busy bit: set while audio plays. Ask the server only if audio was
       playing, so that data reads do not cost an extra request. */
    if (au_state == 1) {
        audio_refresh();
        if (au_state == 1)
            st |= DEV_BUSY;
    }
    FW(rq + 3) = st;
}
