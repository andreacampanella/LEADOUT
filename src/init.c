/* init.c - transient part of the LEADOUT client (C version)
 *
 * Parses the command line, finds the packet driver and a server,
 * links the device driver into DOS and stays resident.
 *
 * Compile with -nt=INIT_TEXT -nc=INIT so that the code goes in INIT_TEXT.
 * The string literals go in CONST. start.asm puts both after the resident
 * part. Do not define global variables with an initial value in this file.
 */
#include "leadout.h"

/* DX:AX / 10 -> quotient in DX:AX, remainder in BX */
u32 div10(u32 v, u16 *rem);
#pragma aux div10 = \
    "push ax" "mov ax,dx" "xor dx,dx" "mov cx,10" "div cx" \
    "mov bx,ax" "pop ax" "div cx" "mov [si],dx" "mov dx,bx" \
    parm [dx ax] [si] value [dx ax] modify exact [ax bx cx dx]

static void dos(struct regs *r)
{
    intcall(0x21, r);
}

static void putch(char c)
{
    struct regs r;
    r.ax = 0x0200;
    r.dx = (u8)c;
    dos(&r);
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putch('\r');
        putch(*s++);
    }
}

static void put_dec(u32 v)
{
    char buf[11];
    int i = 10;
    u16 rem;
    buf[i] = 0;
    do {
        v = div10(v, &rem);
        buf[--i] = (char)('0' + rem);
    } while (v);
    puts_(buf + i);
}

static void put_hex8(u8 v)
{
    static const char hex[] = "0123456789ABCDEF";
    putch(hex[v >> 4]);
    putch(hex[v & 15]);
}

static void put_ip(const u8 *ip)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (i)
            putch('.');
        put_dec(ip[i]);
    }
}

static void quit(u8 code)
{
    struct regs r;
    r.ax = 0x4C00 | code;
    dos(&r);
}

static u8 use_heartbeat = 1;           /* /H:0 turns the timer hook off */

static void usage(void)
{
    puts_("Usage: LEADOUT [a.b.c.d] [/S:a.b.c.d] [/D:name] [/B:n] [/C:n] [/H:0]\n"
          "       LEADOUT /CHECK\n"
          "       LEADOUT /DISC[:n]\n"
          "  a.b.c.d  IP address of this PC. Without it, LEADOUT reads the\n"
          "           IPADDR line from the file in %MTCPCFG% (written by DHCP).\n"
          "           /IP:a.b.c.d also works.\n"
          "  /S:a.b.c.d  use only the server at this address\n"
          "  /D       device name for SHSUCDX (default LEADOUT1)\n"
          "  /B       sectors per request, 1-8 (default 8)\n"
          "  /C       read-ahead buffer in sectors, 0-8 (default 8, 16 KB)\n"
          "  /H:0     no heartbeat from the timer interrupt\n"
          "  /DISC    list the discs on the server and insert one\n"
          "           (/DISC:n inserts disc n, /DISC:0 ejects)\n"
          "  /CHECK   only report what is loaded, as the exit code:\n"
          "           0 no packet driver, 1 packet driver, 2 LEADOUT,\n"
          "           3 LEADOUT and SHSUCDX/MSCDEX\n");
    quit(1);
}

static int parse_num(const char **ps, u16 *out)
{
    const char *s = *ps;
    u16 v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
        n++;
    }
    *ps = s;
    *out = v;
    return n > 0 && n <= 5;
}

/* Parse a.b.c.d at *pp into ip[4]. Returns 1 on success. */
static int parse_ip(const char **pp, u8 *ip)
{
    u16 v;
    int i;
    for (i = 0; i < 4; i++) {
        if (!parse_num(pp, &v) || v > 255)
            return 0;
        ip[i] = (u8)v;
        if (i < 3 && *(*pp)++ != '.')
            return 0;
    }
    return 1;
}

#define OPT_IP     1
#define OPT_CHECK  2
#define OPT_DISC   4

static int disc_arg = -1;               /* /DISC:n (2 bytes in the resident data) */

static int parse_cmdline(void)
{
    char *s = (char *)0x81;             /* PSP command tail */
    char *end = s + *(u8 *)0x80;
    const char *p;
    int flags = 0;
    u16 v;
    int i;

    *end = 0;
    for (p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *(char *)p -= 32;

    p = s;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        if (*p >= '0' && *p <= '9') {           /* address without /IP: */
            if (!parse_ip(&p, my_ip))
                usage();
            flags |= OPT_IP;
            continue;
        }
        if (*p++ != '/')
            usage();
        if (p[0] == 'I' && p[1] == 'P' && p[2] == ':') {
            p += 3;
            if (!parse_ip(&p, my_ip))
                usage();
            flags |= OPT_IP;
        } else if (p[0] == 'S' && (p[1] == ':' || p[1] == '=' || p[1] == ' ')) {
            p += 2;
            while (*p == ' ')
                p++;
            if (!parse_ip(&p, want_srv) || !want_srv[0])
                usage();
        } else if (p[0] == 'C' && p[1] == 'H' && p[2] == 'E' && p[3] == 'C' && p[4] == 'K') {
            p += 5;
            flags |= OPT_CHECK;
        } else if (p[0] == 'D' && p[1] == 'I' && p[2] == 'S' && p[3] == 'C') {
            p += 4;
            flags |= OPT_DISC;
            disc_arg = -1;
            if (*p == ':') {
                p++;
                if (!parse_num(&p, &v))
                    usage();
                disc_arg = v;
            }
        } else if (p[0] == 'D' && p[1] == ':') {
            p += 2;
            for (i = 0; i < 8; i++) {
                if (*p > ' ' && *p != '/')
                    devhdr[10 + i] = *p++;
                else
                    devhdr[10 + i] = ' ';
            }
        } else if (p[0] == 'B' && p[1] == ':') {
            p += 2;
            if (!parse_num(&p, &v) || v < 1 || v > BLOCK_MAX)
                usage();
            block_max = v;
        } else if (p[0] == 'H' && p[1] == ':' && (p[2] == '0' || p[2] == '1')) {
            use_heartbeat = p[2] - '0';
            p += 3;
        } else if (p[0] == 'C' && p[1] == ':') {
            p += 2;
            if (!parse_num(&p, &v) || v > BLOCK_MAX)
                usage();
            cache_sectors = v;
        } else {
            usage();
        }
    }
    return flags;
}

/* Read the IPADDR line from the mTCP configuration file in %MTCPCFG%.
 * path receives the file name (80 bytes). Returns 1 if found. */
static int ip_from_mtcp(char *path)
{
    static const char var[] = "MTCPCFG=";
    char buf[2048];
    struct regs r;
    char far *e;
    u16 envseg = *(u16 *)0x2C;
    u16 n, i;
    char *line;

    path[0] = 0;
    if (!envseg)
        return 0;
    for (e = MK_FP(envseg, 0); *e; ) {
        for (i = 0; var[i] && e[i] == var[i]; i++)
            ;
        if (!var[i]) {
            e += i;
            for (i = 0; i < 79 && e[i]; i++)
                path[i] = e[i];
            path[i] = 0;
            break;
        }
        while (*e)
            e++;
        e++;
    }
    if (!path[0])
        return 0;

    r.ax = 0x3D00;                      /* open, read only */
    r.dx = (u16)path;
    dos(&r);
    if (r.flags & CF)
        return 0;
    r.bx = r.ax;
    r.ax = 0x3F00;                      /* read */
    r.cx = sizeof(buf) - 1;
    r.dx = (u16)buf;
    dos(&r);
    n = (r.flags & CF) ? 0 : r.ax;
    r.ax = 0x3E00;                      /* close */
    dos(&r);
    buf[n] = 0;

    for (line = buf; *line; ) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if ((p[0] | 32) == 'i' && (p[1] | 32) == 'p' && (p[2] | 32) == 'a' &&
            (p[3] | 32) == 'd' && (p[4] | 32) == 'd' && (p[5] | 32) == 'r' &&
            (p[6] == ' ' || p[6] == '\t')) {
            p += 6;
            while (*p == ' ' || *p == '\t')
                p++;
            if (parse_ip(&p, my_ip))
                return 1;
        }
        while (*line && *line != '\n')
            line++;
        if (*line)
            line++;
    }
    return 0;
}

/* Returns the header of a resident LEADOUT with our device name, or 0. */
static u8 far *leadout_loaded(void)
{
    struct regs r;
    u8 far *d;
    int guard, i;

    r.ax = 0x5200;
    dos(&r);
    d = MK_FP(r.es, r.bx + 0x22);       /* NUL device */
    for (guard = 0; guard < 256; guard++) {
        u32 next = *(u32 far *)d;
        if (*(u16 far *)(d + 4) & 0x8000) {     /* character device */
            for (i = 0; i < 8 && d[10 + i] == devhdr[10 + i]; i++)
                ;
            if (i == 8 && d != (u8 far *)devhdr)
                return d;
        }
        if ((u16)next == 0xFFFF)
            break;
        d = (u8 far *)next;
    }
    return 0;
}

/* ---- LEADOUT /DISC: list the library on the server and change the disc ---- */

/* Send a vendor request to the resident driver. Returns the status word. */
static u16 resident_call(u8 far *hdr, u8 sub, u16 arg, void far *buf, u16 size, u16 *outlen)
{
    u8 rq[32];
    int i;
    for (i = 0; i < 32; i++)
        rq[i] = 0;
    rq[0] = 32;
    rq[2] = RQ_LEADOUT;
    rq[0x0D] = sub;
    *(void far **)(rq + 0x0E) = buf;
    W(rq + 0x12) = size;
    W(rq + 0x14) = arg;
    devcall(MK_FP(FP_SEG(hdr), *(u16 far *)(hdr + 6)), (void far *)rq);
    devcall(MK_FP(FP_SEG(hdr), *(u16 far *)(hdr + 8)), (void far *)rq);
    if (outlen)
        *outlen = W(rq + 0x16);
    return W(rq + 3);
}

static int read_line(char *buf, int max)
{
    struct regs r;
    char in[34];
    int i, n;
    in[0] = (char)(max < 32 ? max : 32);
    in[1] = 0;
    r.ax = 0x0A00;
    r.dx = (u16)in;
    dos(&r);
    puts_("\n");
    n = (u8)in[1];
    for (i = 0; i < n; i++)
        buf[i] = in[2 + i];
    buf[n] = 0;
    return n;
}

/* Tell SHSUCDX (or MSCDEX) about the disc change now. SHSUCDX checks for a
 * media change only after 7 seconds without an access. But when a program
 * sends IOCTL 9 through INT 2Fh AX=1510h and the driver reports a change,
 * SHSUCDX re-reads the disc at the next access. */
static void settle(u8 far *hdr)
{
    struct regs r;
    u8 rq[26];
    u8 cb[2];
    u16 drive;
    int i;

    drive = hdr[0x14];                  /* set by SHSUCDX/MSCDEX, 1 = A */
    if (drive) {
        drive--;
    } else {
        r.ax = 0x1500;
        r.bx = 0;
        intcall(0x2F, &r);
        if (!r.bx)
            return;
        drive = r.cx;
    }
    for (i = 0; i < 26; i++)
        rq[i] = 0;
    rq[0] = 26;
    rq[2] = 3;                          /* IOCTL input */
    *(void far **)(rq + 0x0E) = (void far *)cb;
    W(rq + 0x12) = 2;
    cb[0] = 9;                          /* media changed */
    cb[1] = 0;
    r.ax = 0x1510;
    r.cx = drive;
    r.es = FP_SEG((void far *)rq);
    r.bx = (u16)rq;
    intcall(0x2F, &r);
}

static void disc_menu(u8 far *hdr, int direct)
{
    u8 page[1200];
    char line[33];
    u16 start = 0, total, cur, count, len, st, i;
    const u8 *e;

    if (direct >= 0) {                  /* LEADOUT /DISC:n, 0 = eject */
        st = resident_call(hdr, RQ_SELECT, direct ? direct - 1 : 0xFFFF, 0, 0, 0);
        if (st != DEV_DONE)
            puts_("The server could not change the disc.\n");
        else
            puts_(direct ? "Disc changed.\n" : "Disc ejected.\n");
        if (st == DEV_DONE)
            settle(hdr);
        quit(st == DEV_DONE ? 0 : 3);
    }
    for (;;) {
        st = resident_call(hdr, RQ_LIST, start, (void far *)page, sizeof(page), &len);
        if (st != DEV_DONE || len < 7) {
            puts_("The server did not answer. Is it running?\n");
            quit(3);
        }
        total = W(page);
        cur = W(page + 4);
        count = page[6];
        puts_("\nDiscs on the server (");
        put_dec(total);
        puts_("), * = in the drive:\n");
        for (i = 0, e = page + 7; i < count && e < page + len; i++) {
            u8 n = *e++;
            u16 k;
            puts_(start + i == cur ? " *" : "  ");
            if (start + i + 1 < 100) putch(' ');
            if (start + i + 1 < 10) putch(' ');
            put_dec(start + i + 1);
            putch(' ');
            for (k = 0; k < n && k < 70; k++)
                putch(e[k]);
            puts_("\n");
            e += n;
        }
        puts_("Number to insert");
        if (start + count < total)
            puts_(", N = next page");
        if (start)
            puts_(", P = previous page");
        puts_(", E = eject, Enter = quit: ");
        if (!read_line(line, 8))
            quit(0);
        if ((line[0] | 32) == 'n' && start + count < total) {
            start += count;
            continue;
        }
        if ((line[0] | 32) == 'p' && start) {
            start = start >= 20 ? start - 20 : 0;
            continue;
        }
        if ((line[0] | 32) == 'e') {
            st = resident_call(hdr, RQ_SELECT, 0xFFFF, 0, 0, 0);
            puts_(st == DEV_DONE ? "Disc ejected.\n" : "The server could not eject the disc.\n");
            settle(hdr);
            quit(0);
        }
        {
            const char *p = line;
            u16 v;
            if (parse_num(&p, &v) && v >= 1 && v <= total) {
                st = resident_call(hdr, RQ_SELECT, v - 1, 0, 0, 0);
                puts_(st == DEV_DONE ? "Disc changed.\n" : "The server could not load that disc.\n");
                if (st == DEV_DONE)
                    settle(hdr);
                quit(st == DEV_DONE ? 0 : 3);
            }
        }
        puts_("Not a choice.\n");
    }
}

/* Returns 1 if SHSUCDX or MSCDEX is loaded. */
static int cd_extension_loaded(void)
{
    struct regs r;
    r.ax = 0x1500;
    r.bx = 0;
    intcall(0x2F, &r);
    return r.bx != 0;
}

/* Find the interrupt vector of the network card: the IRQ vector that points
 * into the segment of the packet driver. Hook it, so that the heartbeat is
 * not sent while the packet driver works on the card. */
static void hook_nic(void)
{
    struct regs r;
    u16 drv_seg, irq, vec;
    r.ax = 0x3500 | pkt_int;
    dos(&r);
    drv_seg = r.es;
    for (irq = 1; irq < 16; irq++) {
        if (irq == 2)
            continue;                   /* cascade */
        vec = irq < 8 ? 0x08 + irq : 0x68 + irq;
        r.ax = 0x3500 | vec;
        dos(&r);
        if (r.es == drv_seg) {
            old_nic = MK_FP(r.es, r.bx);
            r.ax = 0x2500 | vec;
            r.dx = (u16)nic_isr;
            dos(&r);                    /* DS = our segment */
            puts_("Network card on IRQ ");
            put_dec(irq);
            puts_("\n");
            return;
        }
    }
    puts_("Note: the IRQ of the network card was not found.\n");
}

static int find_pktdrv(void)
{
    static const char sig[] = "PKT DRVR";
    struct regs r;
    u16 n;
    for (n = 0x60; n <= 0x80; n++) {
        u8 far *p;
        int i;
        r.ax = 0x3500 | n;
        dos(&r);
        p = MK_FP(r.es, r.bx + 3);
        for (i = 0; i < 8 && p[i] == sig[i]; i++)
            ;
        if (i == 8) {
            pkt_int = (u8)n;
            return 1;
        }
    }
    return 0;
}

static int access_type(const u8 *type, u16 *handle)
{
    struct regs r;
    r.ax = 0x0201;                      /* access_type, class Ethernet */
    r.bx = 0xFFFF;                      /* any interface type */
    r.dx = 0;                           /* interface number 0 */
    r.si = (u16)type;
    r.cx = 2;
    r.es = FP_SEG((void far *)devhdr);  /* our segment */
    r.di = (u16)receiver;
    if (pktcall(&r))
        return 0;
    *handle = r.ax;
    return 1;
}

static void release_type(u16 handle)
{
    struct regs r;
    if (!handle)
        return;
    r.ax = 0x0300;
    r.bx = handle;
    pktcall(&r);
}

/* LEADOUT /CHECK: report what is loaded. Exit code 0..3 for batch files. */
static void check_and_quit(void)
{
    int level = 0;
    if (find_pktdrv()) {
        level = 1;
        puts_("Packet driver: loaded\n");
    } else {
        puts_("Packet driver: not loaded\n");
    }
    if (leadout_loaded()) {
        level = 2;
        puts_("LEADOUT: loaded\n");
        if (cd_extension_loaded()) {
            level = 3;
            puts_("SHSUCDX/MSCDEX: loaded\n");
        }
    } else {
        puts_("LEADOUT: not loaded\n");
    }
    quit((u8)level);
}

/* The PC's own address: from the command line, or from mTCP's file. */
static void get_own_ip(int flags)
{
    char path[80];
    if (!(flags & OPT_IP)) {
        if (!ip_from_mtcp(path)) {
            puts_("No IP address. Give the address of this PC, for example\n"
                  "  LEADOUT 192.168.1.50\n"
                  "or run mTCP DHCP first, with MTCPCFG set to its configuration file.\n");
            quit(1);
        }
    }
    puts_("This PC: ");
    put_ip(my_ip);
    if (!(flags & OPT_IP)) {
        puts_(" (from ");
        puts_(path);
        puts_(")");
    }
    puts_("\n");
}

/* Open the packet driver for IP and ARP, and read the card's address. */
static void open_packet_driver(void)
{
    static const u8 type_ip[2] = {0x08, 0x00};
    static const u8 type_arp[2] = {0x08, 0x06};
    struct regs r;

    if (!find_pktdrv()) {
        puts_("No packet driver found at INT 60h-80h.\n");
        quit(2);
    }
    puts_("Packet driver at INT ");
    put_hex8(pkt_int);
    puts_("h\n");

    if (!access_type(type_ip, &h_ip) || !access_type(type_arp, &h_arp)) {
        puts_("The packet driver refused access_type.\n");
        release_type(h_ip);
        quit(3);
    }
    r.ax = 0x0600;                      /* get_address */
    r.bx = h_ip;
    r.es = FP_SEG((void far *)devhdr);
    r.di = (u16)my_mac;
    r.cx = 6;
    pktcall(&r);
}

/* Search for the server and show what it serves. */
static void find_server(void)
{
    int i;
    if (want_srv[0]) {
        puts_("Searching for the server at ");
        put_ip(want_srv);
        puts_("...\n");
    } else
        puts_("Searching for a server...\n");
    if (discover(1)) {
        puts_(want_srv[0] ? "That server did not answer.\n" : "No server found.\n");
        if (want_srv[0])
            puts_("Check the address, and that the server is on. The web UI shows it.\n");
        release_type(h_arp);
        release_type(h_ip);
        quit(3);
    }
    puts_("Server ");
    for (i = 0; i < 16 && info.name[i]; i++)
        putch(info.name[i]);
    puts_(" at ");
    put_ip(srv_ip);
    if (info.ntracks) {
        puts_("\nDisc: ");
        put_dec(info.sectors);
        puts_(" sectors, ");
        put_dec(info.ntracks);
        puts_(" track(s)\n");
    } else {
        puts_("\nNo disc selected on the server.\n");
    }
    /* Protocol version: a byte after the track list (0 = version 1 server). */
    i = info.toc[info.ntracks * 6];
    if (i > PROTO_VERSION) {
        puts_("Note: the server uses a newer protocol (version ");
        put_dec(i);
        puts_("). Update LEADOUT.COM.\n");
    }
}

/* Put the device driver into the DOS chain, hook the timer, stay resident. */
static void install(void)
{
    struct regs r;
    u8 far *nul;
    int i;

    /* Link the device after the NUL device (list of lists + 22h). */
    r.ax = 0x5200;
    dos(&r);
    nul = MK_FP(r.es, r.bx + 0x22);
    cli();
    *(u32 *)devhdr = *(u32 far *)nul;
    *(void far * far *)nul = (void far *)devhdr;
    sti();

    /* Timer hook for the heartbeat. /H:0 leaves the timer alone. */
    if (use_heartbeat) {
        r.ax = 0x351C;
        dos(&r);
        old_1c = MK_FP(r.es, r.bx);
        r.ax = 0x251C;
        r.dx = (u16)timer_isr;
        dos(&r);                        /* DS = our segment */
        hook_nic();
    } else {
        hb_enabled = 0;
    }

    puts_("Installed as device ");
    for (i = 0; i < 8 && devhdr[10 + i] != ' '; i++)
        putch(devhdr[10 + i]);
    puts_(" (load SHSUCDX or MSCDEX with /D:");
    for (i = 0; i < 8 && devhdr[10 + i] != ' '; i++)
        putch(devhdr[10 + i]);
    puts_(")\n");

    /* Free the environment and stay resident. */
    r.ax = 0x4900;
    r.es = *(u16 *)0x2C;
    dos(&r);
    r.ax = 0x3100;
    r.dx = ((u16)&resident_end + 15) >> 4;
    if (cache_sectors > block_max)      /* one block is one request */
        cache_sectors = block_max;
    if (cache_sectors) {                /* the read-ahead buffer, after the resident part */
        cache_off = r.dx << 4;
        r.dx += cache_sectors * (2048 / 16);
    }
    dos(&r);
}

void init_main(void)
{
    int flags;

    puts_("LEADOUT " VERSION_STR " - network CD-ROM client\n");
    flags = parse_cmdline();

    if (flags & OPT_CHECK)
        check_and_quit();

    if (flags & OPT_DISC) {
        u8 far *hdr = leadout_loaded();
        if (!hdr) {
            puts_("LEADOUT is not loaded. Run MOUNT first.\n");
            quit(1);
        }
        disc_menu(hdr, disc_arg);       /* does not return */
    }

    if (leadout_loaded()) {
        puts_("LEADOUT is already loaded.\n");
        quit(4);
    }
    get_own_ip(flags);
    open_packet_driver();
    find_server();
    install();
}
