/* net.c - the resident network part of the LEADOUT client
 *
 * Ethernet frames to and from the packet driver, ARP, IPv4 and UDP, and
 * the LEADIN protocol: search for a server, read sectors, send a control
 * request and wait for its reply. The receiver (start.asm) calls
 * rx_process() for each frame with DS = SS = DGROUP.
 *
 * All resident variables are initialized, so they stay in _DATA.
 */
#include "leadout.h"

u8  my_mac[6] = {0};
u8  my_ip[4] = {0};
u8  srv_mac[6] = {0};
u8  srv_ip[4] = {0};
u8  want_srv[4] = {0};                /* /S:a.b.c.d, 0 = any server */
u16 h_ip = 0, h_arp = 0;
u16 block_max = BLOCK_MAX;              /* /B:n, sectors per request */
u8  pkt_int = 0x60;

static u16 seq = 0;
static u16 ip_id = 0;

static volatile u8  rx_mode = 0;        /* 0 idle, 1 discover, 2 read, 3 control */
static volatile u8  reply_ok = 0;
static volatile u8  rd_err = 0;
static volatile u16 rd_mask = 0;
static u8  rd_last = 0;                 /* the last part the server will send for this request */
static volatile u8 rd_short = 0;        /* 1: the last part came and parts are still missing */
static u16 rd_want = 0;
static u8 far *rd_buf = 0;

/* Reply to a control request (rx_mode 3) */
static volatile u8 rep_type = 0;
static volatile u8 rep_status = 0;
u8  rep_buf[16] = {0};
u8 far *rep_dst = 0;
u16 rep_max = 0;
u16 rep_len = 0;

/* Heartbeat (protocol version 2) */
u8  hb_active = 0;
u8  hb_enabled = 1;                     /* 0 = /H:0 */
u8  hb_ticks = 0;
u8  hb_stack[256] = {0};
#pragma aux hb_stack "*"

/* ARP: a request to answer, and the reply we wait for */
static volatile u8 arp_pend = 0;
static u8 arp_mac[6] = {0};
static u8 arp_ip[4] = {0};
static volatile u8 arp_got = 0;         /* an ARP reply from want_srv arrived */
static volatile u8 ann_want = 0;        /* searching: accept ANNOUNCE from a server */
static volatile u8 ann_got = 0;         /* an ANNOUNCE set srv_mac and srv_ip */

u8 txbuf[128] = {0};                    /* the largest frame sent is 60 bytes */
u8 rxbuf[RX_MAX] = {0};
#pragma aux rxbuf "*"

/* Private stacks for the assembly stubs (sizes must match start.asm) */
u8 dev_stack[768] = {0};
#pragma aux dev_stack "*"
u8 rx_stack[256] = {0};
#pragma aux rx_stack "*"

/* The BIOS timer, 18.2 ticks per second. Not static: the emulator test
 * finds it in the map file. */
u16 ticks(void)
{
    return *(volatile u16 far *)MK_FP(0, 0x46C);
}

int pktcall(struct regs *r)
{
    intcall(pkt_int, r);
    return r->flags & CF;
}

/* Interrupt controller in-service register. Call with interrupts off. */
static u8 pic_isr(u16 port)
{
    u8 v;
    outp(port, 0x0B);                   /* OCW3: read the in-service register */
    v = inp(port);
    outp(port, 0x0A);                   /* back to the interrupt request register */
    return v;
}

/* Called from the INT 1Ch hook (start.asm) 18.2 times per second, with
 * interrupts off, on the heartbeat stack. */
void timer_c(void)
{
    if (!hb_active || !hb_enabled)
        return;
    if (net_busy)                       /* LEADOUT is in a request or a receive */
        return;
    /* Another interrupt is in service, or the packet driver is in its own
     * interrupt code: the packet driver must not be entered again. */
    if (nic_busy || (pic_isr(0x20) & 0xFE) || pic_isr(0xA0))
        return;
    net_busy++;
    heartbeat_tick();
    net_busy--;
}

static int ip_equal(const u8 *a, const u8 *b)
{
    return W(a) == W(b) && W(a + 2) == W(b + 2);
}

/* ------------------------------------------------------------------------
 * Receive: called by the packet driver upcall for each complete frame.
 * ---------------------------------------------------------------------- */
void rx_process(u16 len)
{
    u16 t = W(rxbuf + ETH_TYPE);

    if (t == ETH_ARP_N) {
        if (len < 42)
            return;
        if (W(rxbuf + 20) == ARP_REPLY_N) {     /* the server we asked for? */
            if (want_srv[0] && ip_equal(rxbuf + 28, want_srv)) {
                ncopy(srv_mac, rxbuf + 22, 6);
                arp_got = 1;
            }
            return;
        }
        if (W(rxbuf + 20) != ARP_REQUEST_N)
            return;
        if (!ip_equal(rxbuf + 38, my_ip) || arp_pend)
            return;
        ncopy(arp_mac, rxbuf + 22, 6);
        ncopy(arp_ip, rxbuf + 28, 4);
        arp_pend = 1;
        return;
    }
    if (t != ETH_IP_N || len < NCD_BODY)
        return;
    if (rxbuf[IP_HDR] != 0x45 || rxbuf[IP_HDR + 9] != 17)
        return;
    if (W(rxbuf + IP_HDR + 6) & 0xFF3F)         /* fragment */
        return;
    if (W(rxbuf + UDP_DPORT) != PORT_N)
        return;
    if (W(rxbuf + NCD_HDR) != 0x434E || W(rxbuf + NCD_HDR + 2) != 0x0144)  /* "NCD",1 */
        return;
    if (rxbuf[NCD_HDR + 4] == T_ANNOUNCE) {     /* broadcast from a server */
        if (!ann_want || ann_got)
            return;
        if (want_srv[0] && !ip_equal(rxbuf + IP_SRC, want_srv))
            return;
        ncopy(srv_mac, rxbuf + ETH_SRC, 6);
        ncopy(srv_ip, rxbuf + IP_SRC, 4);
        ann_got = 1;
        return;
    }
    if (!ip_equal(rxbuf + IP_DST, my_ip))
        return;
    if (W(rxbuf + NCD_HDR + 6) != seq)
        return;

    if (rxbuf[NCD_HDR + 4] == (T_DISCOVER | T_REPLY)) {     /* OFFER */
        u16 n;
        if (rx_mode != 1 || reply_ok)
            return;
        if (want_srv[0] && !ip_equal(rxbuf + IP_SRC, want_srv))
            return;                             /* another server */
        n = SWAP16(W(rxbuf + UDP_LEN));
        if (n <= 16)
            return;
        n -= 16;
        if (n > sizeof(info))
            n = sizeof(info);
        ncopy(srv_mac, rxbuf + ETH_SRC, 6);
        ncopy(srv_ip, rxbuf + IP_SRC, 4);
        fzero((void far *)&info, sizeof(info));
        ncopy(&info, rxbuf + NCD_BODY, n);
        reply_ok = 1;
    } else if (rxbuf[NCD_HDR + 4] == (T_READ | T_REPLY)) {  /* DATA */
        u16 part, bit;
        if (rx_mode != 2 || !ip_equal(rxbuf + IP_SRC, srv_ip))
            return;
        if (rxbuf[NCD_HDR + 5] != 0) {
            rd_err = rxbuf[NCD_HDR + 5];
            return;
        }
        if (SWAP16(W(rxbuf + UDP_LEN)) < 8 + 12 + 1024)
            return;
        part = rxbuf[NCD_BODY];
        if (part >= 16)
            return;
        bit = 1u << part;
        if (!(rd_want & bit) || (rd_mask & bit))
            return;
        fcopy(rd_buf + (part << 10), rxbuf + NCD_BODY + 4, 1024);
        rd_mask |= bit;
        /* The server sends the parts in order. When the last one is here and
         * some are missing, they were lost: no need to wait for the timer. */
        if (part == rd_last && rd_mask != rd_want)
            rd_short = 1;
    } else if (rx_mode == 3 && rxbuf[NCD_HDR + 4] == rep_type && !reply_ok) {
        u16 n;
        if (!ip_equal(rxbuf + IP_SRC, srv_ip))
            return;
        n = SWAP16(W(rxbuf + UDP_LEN)) - 16;
        if (rep_dst) {
            if (n > rep_max)
                n = rep_max;
            fcopy(rep_dst, rxbuf + NCD_BODY, n);
        } else {
            if (n > sizeof(rep_buf))
                n = sizeof(rep_buf);
            ncopy(rep_buf, rxbuf + NCD_BODY, n);
        }
        rep_len = n;
        rep_status = rxbuf[NCD_HDR + 5];
        reply_ok = 1;
    }
}

/* ------------------------------------------------------------------------
 * Transmit
 * ---------------------------------------------------------------------- */
static void send_frame(u16 len)
{
    struct regs r;
    r.ax = 0x0400;
    r.si = (u16)txbuf;
    r.cx = len < 60 ? 60 : len;
    pktcall(&r);
}

/* Send an ARP frame. dst_mac 0 = broadcast. target_mac 0 = unknown. */
static void send_arp(u16 opcode, const u8 *dst_mac, const u8 *target_mac, const u8 *target_ip)
{
    u8 *p = txbuf;
    if (dst_mac)
        ncopy(p, dst_mac, 6);
    else
        W(p) = W(p + 2) = W(p + 4) = 0xFFFF;
    ncopy(p + 6, my_mac, 6);
    W(p + 12) = ETH_ARP_N;
    W(p + 14) = 0x0100;             /* hardware type Ethernet */
    W(p + 16) = 0x0008;             /* protocol IPv4 */
    W(p + 18) = 0x0406;             /* address lengths 6 and 4 */
    W(p + 20) = opcode;
    ncopy(p + 22, my_mac, 6);
    ncopy(p + 28, my_ip, 4);
    if (target_mac)
        ncopy(p + 32, target_mac, 6);
    else
        fzero(p + 32, 6);
    ncopy(p + 38, target_ip, 4);
    send_frame(42);
}

/* Send an ARP reply if the receiver queued an ARP request. */
static void service(void)
{
    if (!arp_pend)
        return;
    send_arp(ARP_REPLY_N, arp_mac, arp_mac, arp_ip);
    arp_pend = 0;
}

static void ncd_header(u8 type)
{
    W(txbuf + NCD_HDR) = 0x434E;    /* "NC" */
    W(txbuf + NCD_HDR + 2) = 0x0144; /* "D", version 1 */
    txbuf[NCD_HDR + 4] = type;
    txbuf[NCD_HDR + 5] = 0;
    W(txbuf + NCD_HDR + 6) = seq;
}

/* Send the NCD payload at txbuf+42. len = payload length. */
static void send_udp(u16 len, int bcast)
{
    u8 *p = txbuf;
    u16 i;
    u32 sum = 0;

    if (bcast) {
        W(p) = W(p + 2) = W(p + 4) = 0xFFFF;
    } else {
        ncopy(p, srv_mac, 6);
    }
    ncopy(p + 6, my_mac, 6);
    W(p + 12) = ETH_IP_N;
    p += IP_HDR;
    W(p) = 0x0045;
    W(p + 2) = SWAP16(len + 28);
    W(p + 4) = ++ip_id;
    W(p + 6) = 0;
    W(p + 8) = 0x1140;              /* TTL 64, UDP */
    W(p + 10) = 0;
    ncopy(p + 12, my_ip, 4);
    if (bcast)
        W(p + 16) = W(p + 18) = 0xFFFF;
    else
        ncopy(p + 16, srv_ip, 4);
    for (i = 0; i < 20; i += 2)
        sum += W(p + i);
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += sum >> 16;
    W(p + 10) = (u16)~sum;
    p += 20;
    W(p) = PORT_N;
    W(p + 2) = PORT_N;
    W(p + 4) = SWAP16(len + 8);
    W(p + 6) = 0;
    send_frame(len + 42);
}

/* Called from the INT 1Ch hook (start.asm), about 18.2 times per second,
 * only when it is safe to use the packet driver. Sends HEARTBEAT every
 * 5 seconds while a play started by DOS is active. */
void heartbeat_tick(void)
{
    if (++hb_ticks < 91)
        return;
    hb_ticks = 0;
    ncd_header(T_HEARTBEAT);
    send_udp(8, 0);
}

/* ------------------------------------------------------------------------
 * Protocol
 * ---------------------------------------------------------------------- */
/* Find the hardware address of want_srv: ask with ARP, or take it from the
 * server's ANNOUNCE. Returns 0 on success. Needed to send to the server
 * directly: a Wi-Fi access point may not pass a broadcast on to the server,
 * but the server's own broadcasts do arrive. */
static int arp_resolve(void)
{
    int tries;
    arp_got = 0;
    ann_got = 0;
    ann_want = 1;
    for (tries = 0; tries < 3; tries++) {
        u16 t0;
        send_arp(ARP_REQUEST_N, 0, 0, want_srv);
        t0 = ticks();
        do {
            sti();
            service();
            if (arp_got || ann_got) {
                ncopy(srv_ip, want_srv, 4);
                ann_want = 0;
                return 0;
            }
        } while ((u16)(ticks() - t0) < 18);
    }
    ann_want = 0;
    return -1;
}

/* Find a server (bcast = 1), or get the disc info again from the known
 * server (bcast = 0). With want_srv set, the search goes to that server
 * directly, not as a broadcast. Returns 0 on success. */
int discover(int bcast)
{
    int tries;
    if (bcast && want_srv[0]) {
        if (arp_resolve())
            return -1;
        bcast = 0;
    }
    reply_ok = 0;
    ann_got = 0;
    ann_want = bcast;
    rx_mode = 1;
    for (tries = 0; tries < (bcast ? 4 : RETRIES); tries++) {
        u16 t0;
        ++seq;
        ncd_header(T_DISCOVER);
        send_udp(8, bcast);
        t0 = ticks();
        do {
            sti();
            service();
            if (reply_ok) {
                rx_mode = 0;
                ann_want = 0;
                return 0;
            }
            if (ann_got) {              /* a server announced itself: ask it directly */
                ann_got = 0;
                ncd_header(T_DISCOVER);
                send_udp(8, 0);
            }
        } while ((u16)(ticks() - t0) < (bcast ? 18 : TIMEOUT_TICKS));
    }
    rx_mode = 0;
    ann_want = 0;
    return -1;
}

/* Read count sectors (1..8) to buf. Returns 0 on success, -1 if no
 * reply came, or the error status from the server (2 = the disc changed,
 * 3 = no disc). */
int net_read(u32 lba, u8 count, u8 far *buf)
{
    u16 t_begin;
    int tries;
    rd_buf = buf;
    rd_want = (count >= 8) ? 0xFFFF : (u16)((1u << (count * 2)) - 1);
    rd_mask = 0;
    rd_err = 0;
    ++seq;
    rx_mode = 2;
    /* A read gives up when it has used both its tries and its time (about a
     * second). The time alone is not enough: an early re-request after a
     * short burst would use up six tries in a few milliseconds when the air
     * is bad for a moment. The tries alone are not enough either, with a
     * slow server. */
    t_begin = ticks();
    for (tries = 0; ; tries++) {
        u16 t0, missing, m;
        u8 last;
        if (tries >= RETRIES && (u16)(ticks() - t_begin) >= READ_TICKS)
            break;
        missing = rd_want & ~rd_mask;           /* missing parts only */
        for (last = 15, m = 0x8000; !(missing & m); m >>= 1)
            last--;
        rd_last = last;
        rd_short = 0;
        ncd_header(T_READ);
        *(u32 *)(txbuf + NCD_BODY) = lba;
        txbuf[NCD_BODY + 4] = count;
        W(txbuf + NCD_BODY + 5) = missing;
        txbuf[NCD_BODY + 7] = info.disc_id;
        send_udp(16, 0);
        t0 = ticks();
        do {
            sti();
            service();
            if (rd_mask == rd_want) {
                rx_mode = 0;
                return 0;
            }
            if (rd_err) {
                rx_mode = 0;
                return rd_err;
            }
            if (rd_short)
                break;                          /* ask again at once */
        } while ((u16)(ticks() - t0) < TIMEOUT_TICKS);
    }
    rx_mode = 0;
    return -1;
}

/* Send a control request. The body (body_len bytes) is already at
 * txbuf+NCD_BODY. Returns the reply status, or -1 if no reply came. */
int ncd_request(u8 type, u16 body_len)
{
    int tries;
    ++seq;
    rep_type = type | T_REPLY;
    reply_ok = 0;
    rx_mode = 3;
    for (tries = 0; tries < RETRIES; tries++) {
        u16 t0;
        ncd_header(type);
        send_udp(8 + body_len, 0);
        t0 = ticks();
        do {
            sti();
            service();
            if (reply_ok) {
                rx_mode = 0;
                return rep_status;
            }
        } while ((u16)(ticks() - t0) < TIMEOUT_TICKS);
    }
    rx_mode = 0;
    return -1;
}
