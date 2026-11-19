; start.asm - entry point and low-level stubs for LEADOUT (C version)
;
; Assemble: nasm -f obj -o start.obj start.asm
;
; This file defines the segment order of the program:
;   _TEXT _BSS _DATA                  resident part
;   RESEND                            marks the end of the resident part
;   CONST CONST2 INIT_TEXT            transient part (init strings and code)
; The resident C files (net.c, cdrom.c) must not use string literals or const data,
; because the compiler puts those in CONST. build.sh checks this.
; All segments are in DGROUP. The .COM file is linked as a raw binary at 100h.

        cpu     8086

DEV_STACK_SIZE  equ     768             ; must match dev_stack[] in net.c
RX_STACK_SIZE   equ     256             ; must match rx_stack[] in net.c
HB_STACK_SIZE   equ     256             ; must match hb_stack[] in net.c
RX_MAX          equ     1514

segment _TEXT   public class=CODE   align=1  use16
segment _BSS    public class=BSS    align=2  use16
segment _DATA   public class=DATA   align=2  use16
segment RESEND  public class=DATA   align=16 use16
segment CONST   public class=DATA   align=2  use16
segment CONST2  public class=DATA   align=2  use16
segment INIT_TEXT public class=INIT align=1  use16
group DGROUP _TEXT _BSS _DATA RESEND CONST CONST2 INIT_TEXT

extern rx_process, dev_request, init_main, timer_c
extern req_ptr, rxbuf, dev_stack, rx_stack, hb_stack
global devhdr, intcall, receiver, resident_end, timer_isr, old_1c, net_busy
global nic_isr, old_nic, nic_busy

; ----------------------------------------------------------------------------
segment _TEXT
start:
        jmp     init_entry

        align   2
devhdr:
        dd      -1                      ; next driver
        dw      0C800h                  ; character device, IOCTL, open/close
        dw      strategy wrt DGROUP
        dw      interrupt wrt DGROUP
        db      'LEADOUT1'              ; device name (init can change it)
        dw      0                       ; reserved
        db      0                       ; drive letter (set by SHSUCDX/MSCDEX)
        db      1                       ; number of units

; Device driver strategy routine: save the request header address.
strategy:
        mov     [cs:req_ptr wrt DGROUP], bx
        mov     [cs:(req_ptr+2) wrt DGROUP], es
        retf

; Device driver interrupt routine: switch to a private stack, call C.
interrupt:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        cli
        mov     [dev_old_ss], ss
        mov     [dev_old_sp], sp
        mov     ss, ax
        mov     sp, (dev_stack+DEV_STACK_SIZE) wrt DGROUP
        inc     byte [net_busy]
        sti
        cld
        call    dev_request
        cli
        dec     byte [net_busy]
        mov     ss, [dev_old_ss]
        mov     sp, [dev_old_sp]
        sti
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

; Packet driver receive upcall.
;   AX=0: CX = length. Return ES:DI = buffer, or 0:0 to drop the frame.
;   AX=1: the frame is complete in the buffer. CX = length.
receiver:
        or      ax, ax
        jnz     .complete
        cmp     cx, RX_MAX
        ja      .drop
        push    cs
        pop     es
        mov     di, rxbuf wrt DGROUP
        retf
.drop:
        xor     di, di
        mov     es, di
        retf
.complete:
        pushf
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        cli
        mov     [rx_old_ss], ss
        mov     [rx_old_sp], sp
        mov     ss, ax
        mov     sp, (rx_stack+RX_STACK_SIZE) wrt DGROUP
        cld
        inc     byte [net_busy]
        mov     ax, cx
        call    rx_process
        dec     byte [net_busy]
        cli
        mov     ss, [rx_old_ss]
        mov     sp, [rx_old_sp]
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        popf
        retf

; INT 1Ch hook: chain to the old handler, then run timer_c (net.c) on a
; private stack. timer_c sends the heartbeat when it is safe. in_tick stops
; a second entry while the first one runs.
timer_isr:
        pushf
        call    far [cs:old_1c wrt DGROUP]
        cmp     byte [cs:in_tick wrt DGROUP], 0
        jne     .out
        inc     byte [cs:in_tick wrt DGROUP]
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     [hb_old_ss], ss
        mov     [hb_old_sp], sp
        mov     ss, ax
        mov     sp, (hb_stack+HB_STACK_SIZE) wrt DGROUP
        cld
        call    timer_c
        mov     ss, [hb_old_ss]
        mov     sp, [hb_old_sp]
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        dec     byte [cs:in_tick wrt DGROUP]
.out:
        iret

; Hook on the interrupt vector of the network card. The packet driver can
; send the end-of-interrupt and enable interrupts before it is done with the
; card, so the in-service register does not show it. nic_busy does.
nic_isr:
        inc     byte [cs:nic_busy wrt DGROUP]
        pushf
        call    far [cs:old_nic wrt DGROUP]
        dec     byte [cs:nic_busy wrt DGROUP]
        iret

; void intcall(u8 intno [AL], struct regs *r [DX])
; Loads AX BX CX DX SI DI ES from *r, does INT intno, and stores the
; registers and the flags back. DS must be DGROUP. Keeps all registers
; except AX.
intcall:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    es
        mov     [cs:intno wrt DGROUP], al
        mov     [rptr], dx
        mov     bp, dx
        mov     ax, [ds:bp]
        mov     bx, [ds:bp+2]
        mov     cx, [ds:bp+4]
        mov     dx, [ds:bp+6]
        mov     si, [ds:bp+8]
        mov     di, [ds:bp+10]
        mov     es, [ds:bp+12]
        jmp     short .go               ; flush the prefetch queue
.go:
        db      0CDh                    ; INT intno
intno:  db      0
        pushf
        mov     bp, [rptr]
        mov     [ds:bp], ax
        mov     [ds:bp+2], bx
        mov     [ds:bp+4], cx
        mov     [ds:bp+6], dx
        mov     [ds:bp+8], si
        mov     [ds:bp+10], di
        mov     [ds:bp+12], es
        pop     ax
        mov     [ds:bp+14], ax
        pop     es
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

; ----------------------------------------------------------------------------
segment _DATA
dev_old_ss      dw      0
dev_old_sp      dw      0
rx_old_ss       dw      0
rx_old_sp       dw      0
rptr            dw      0
hb_old_ss       dw      0
hb_old_sp       dw      0
old_1c          dd      0
old_nic         dd      0
net_busy        db      0
nic_busy        db      0
in_tick         db      0

; ----------------------------------------------------------------------------
segment RESEND
resident_end:

; ----------------------------------------------------------------------------
segment INIT_TEXT
; void devcall(void far *routine [DX:AX], void far *req [ES:BX])
; Far-calls a device driver routine (strategy or interrupt) with ES:BX.
global devcall
devcall:
        push    bp
        push    si
        push    di
        push    ds
        push    dx
        push    ax
        mov     bp, sp
        call    far [ss:bp]
        add     sp, 4
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret

init_entry:
        call    init_main
        mov     ax, 4C00h
        int     21h
