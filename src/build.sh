#!/bin/sh
# Build LEADOUT.COM (C version) with OpenWatcom and NASM.
set -e
cd "$(dirname "$0")"
CC16=${CC16:-wcc}
LINK16=${LINK16:-wlink}
OPTS="-q -0 -ms -s -os -zl -wx -we -fpc -bt=dos"
nasm -f obj -o start.obj start.asm
$CC16 $OPTS -fo=net.obj net.c
$CC16 $OPTS -fo=cdrom.obj cdrom.c
$CC16 $OPTS -nt=INIT_TEXT -nc=INIT -fo=init.obj init.c
$LINK16 option quiet format raw bin option offset=0x100 option nodefaultlibs \
    option map=leadout.map name LEADOUT.COM file start.obj,net.obj,cdrom.obj,init.obj
# The resident part must not contain CONST data (see start.asm).
awk '/^RESEND/{r=$4} /^CONST /{c=$4} END{ if (c < r) { print "error: CONST is before RESEND"; exit 1 } }' leadout.map
ls -l LEADOUT.COM
