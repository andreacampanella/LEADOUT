/* leadout.h - shared declarations for the LEADOUT client
 *
 * Files:
 *   start.asm   device driver entry, packet driver upcall, timer hook, stacks
 *   net.c       resident: Ethernet, ARP, IP, UDP and the LEADIN protocol
 *   cdrom.c     resident: the MSCDEX device driver requests, CD audio state
 *   init.c      transient: command line, DHCP address, install, LEADOUT /DISC
 */
#ifndef LEADOUT_H
#define LEADOUT_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define VERSION_STR     "0.16"
#define PROTO_VERSION   2
#define PORT_N          0x3009u         /* port 2352 in network byte order */
#define TOC_MAX         99
#define RX_MAX          1514
#define TIMEOUT_TICKS   3               /* about 165 ms */
#define RETRIES         6
#define READ_TICKS      (RETRIES * TIMEOUT_TICKS)   /* a read gives up after about 1 s */
#define CACHE_TICKS     18              /* a read-ahead block is good for about 1 s */
#define BLOCK_MAX       8               /* most sectors per READ request (16 parts of 1 KB) */

/* LEADIN protocol (PROTOCOL.md). A reply has bit 7 set. */
#define T_DISCOVER      0x01
#define T_READ          0x02
#define T_PLAY          0x03
#define T_STOP          0x04
#define T_RESUME        0x05
#define T_ASTATUS       0x06
#define T_VOLUME        0x07
#define T_MEDIA         0x08
#define T_HEARTBEAT     0x09
#define T_LIST          0x0A
#define T_SELECT        0x0B
#define T_ANNOUNCE      0x0C
#define T_REPLY         0x80
#define ST_OK           0
#define ST_ERROR        1
#define ST_CHANGED      2               /* the disc changed: new disc ID */
#define ST_NODISC       3

/* MSCDEX device driver: request commands, and the status word */
#define CMD_IOCTL_IN    3
#define CMD_IN_FLUSH    7
#define CMD_OUT_FLUSH   11
#define CMD_IOCTL_OUT   12
#define CMD_OPEN        13
#define CMD_CLOSE       14
#define CMD_READ_LONG   128
#define CMD_PREFETCH    130
#define CMD_SEEK        131
#define CMD_PLAY        132
#define CMD_STOP        133
#define CMD_RESUME      136
#define DEV_DONE        0x0100          /* done, no error */
#define DEV_BUSY        0x0200          /* audio is playing */
#define DEV_ERR_NOT_READY   0x8102
#define DEV_ERR_UNKNOWN_CMD 0x8103
#define DEV_ERR_NOT_FOUND   0x8108
#define DEV_ERR_READ        0x810B
#define DEV_ERR_GENERAL     0x810C
#define DEV_ERR_DISC_CHANGE 0x810F

/* Ethernet and ARP constants, in network byte order */
#define ETH_IP_N        0x0008
#define ETH_ARP_N       0x0608
#define ARP_REQUEST_N   0x0100
#define ARP_REPLY_N     0x0200

/* Offsets in an Ethernet frame */
#define ETH_SRC         6
#define ETH_TYPE        12
#define IP_HDR          14
#define IP_SRC          26
#define IP_DST          30
#define UDP_DPORT       36
#define UDP_LEN         38
#define NCD_HDR         42
#define NCD_BODY        50

/* Far pointer helpers (the same as in the OpenWatcom i86.h) */
#define FP_OFF(p)   ((u16)(p))
#define FP_SEG(p)   ((u16)((u32)(void __far *)(p) >> 16))
#define MK_FP(s, o) (((u16)(s)) :> ((void __near *)(o)))

#define W(p)    (*(u16 *)(p))
u8 inp(u16 port);
#pragma aux inp = "in al, dx" parm [dx] value [al];
void outp(u16 port, u8 v);
#pragma aux outp = "out dx, al" parm [dx] [al];

#define FW(p)   (*(u16 far *)(p))
#define FL(p)   (*(u32 far *)(p))
#define SWAP16(x) ((u16)(((x) << 8) | ((x) >> 8)))

/* Register block for intcall(). DS is always DGROUP. */
struct regs {
    u16 ax, bx, cx, dx, si, di, es, flags;
};
#define CF 1

/* ---- assembly helpers (start.asm) ---- */
extern void intcall(u16 intno, struct regs *r);
#pragma aux intcall "*" parm [ax] [dx]
extern void receiver(void);                 /* packet driver upcall entry */
#pragma aux receiver "*"
extern u8 devhdr[18];                       /* device driver header */
#pragma aux devhdr "*"
extern u8 resident_end;                     /* first byte after the resident part */
#pragma aux resident_end "*"

/* ---- inline helpers ---- */
void ncopy(void *d, const void *s, u16 n);
#pragma aux ncopy = \
    "push es" "push ds" "pop es" "cld" "rep movsb" "pop es" \
    parm [di] [si] [cx] modify exact [di si cx]
void fcopy(void far *d, const void *s, u16 n);
#pragma aux fcopy = "cld" "rep movsb" \
    parm [es di] [si] [cx] modify exact [di si cx]
void fzero(void far *d, u16 n);
#pragma aux fzero = "cld" "xor al,al" "rep stosb" \
    parm [es di] [cx] modify exact [di cx al]
u16 div32(u32 v, u16 d, u16 *rem);     /* quotient must fit in 16 bits */
#pragma aux div32 = "div cx" "mov [bx],dx" \
    parm [dx ax] [cx] [bx] value [ax] modify exact [ax dx]
u32 mul16(u16 a, u16 b);
#pragma aux mul16 = "mul dx" parm [ax] [dx] value [dx ax] modify exact [ax dx]
void sti(void);
#pragma aux sti = "sti"
void cli(void);
#pragma aux cli = "cli"

/* ---- resident data ---- */
struct info {                           /* the disc, as the server's OFFER describes it */
    char name[16];
    u32  sectors;
    u8   first, last, ntracks, disc_id;
    u8   leadout[4];
    u8   toc[TOC_MAX * 6];
};
extern struct info info;                /* cdrom.c */
extern u16 cache_sectors, cache_off;    /* cdrom.c: the read-ahead buffer */
extern u8  my_mac[6], my_ip[4], srv_ip[4], want_srv[4];     /* net.c */
extern u16 h_ip, h_arp, block_max;
extern u8  pkt_int;
extern u8  txbuf[];                     /* the frame being built; body at txbuf + NCD_BODY */
extern u8  rep_buf[16];                 /* body of the last control reply */
extern u8 far *rep_dst;                 /* where a long reply body goes, 0 = rep_buf */
extern u16 rep_max, rep_len;

/* Heartbeat: the timer hook (start.asm) calls timer_c(). nic_isr wraps the
 * network card's interrupt and sets nic_busy while it runs. */
extern void timer_isr(void);
#pragma aux timer_isr "*"
extern void far *old_1c;
#pragma aux old_1c "*"
extern void nic_isr(void);
#pragma aux nic_isr "*"
extern void far *old_nic;
#pragma aux old_nic "*"
extern volatile u8 net_busy, nic_busy;
#pragma aux net_busy "*"
#pragma aux nic_busy "*"
extern u8 hb_active;                    /* 1 while a play started by DOS is active */
#pragma aux hb_active "*"
extern u8 hb_ticks;
extern u8 hb_enabled;                   /* 0 = /H:0 */
void timer_c(void);
#pragma aux timer_c "*"
void heartbeat_tick(void);
#pragma aux heartbeat_tick "*"

/* Vendor request to the resident driver (LEADOUT /DISC) */
#define RQ_LEADOUT      0xF0
#define RQ_LIST         1
#define RQ_SELECT       2
extern void far *req_ptr;
#pragma aux req_ptr "*"
void devcall(void far *routine, void far *req);
#pragma aux devcall "*" parm [dx ax] [es bx] modify [ax bx cx dx es]

/* ---- net.c ---- */
u16  ticks(void);
#pragma aux ticks "*"
int  pktcall(struct regs *r);
void rx_process(u16 len);
#pragma aux rx_process "*" parm [ax]
int  discover(int bcast);
int  net_read(u32 lba, u8 count, u8 far *buf);
int  ncd_request(u8 type, u16 body_len);

/* ---- cdrom.c ---- */
void dev_request(void);
#pragma aux dev_request "*"

/* ---- init.c ---- */
void init_main(void);
#pragma aux init_main "*"

#endif
