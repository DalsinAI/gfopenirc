#!/bin/sh
# GFOpenIRC for AmigaOS 3.x: the Shell client build-amiga/GFOpenIRC, with
# the os32 GCC 16 stove (amiga-gcc, libnix), 68040, no FPU. It runs
# gfopennetkit's build-amiga.sh first and links its library.
#   STOVE    the stove (default ~/AmiCradle/stoves/os32-gcc16-open6)
#   NETKIT   a gfopennetkit checkout (default ../gfopennetkit)
#   TLS      a gfopentls checkout for its headers (default ../gfopentls)
#   KEYS     a gfopenkeys checkout for its headers (default ../gfopenkeys);
#            without one the client is built with no keystore (no SASL)
#   OUT      the output directory (default build-amiga)
# MIT licence, (c) 2026 AmiCradle Team.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16-open6"}
export STOVE
P="$STOVE/prefix"
CC=${CC:-"$P/bin/m68k-amigaos-gcc"}
NETKIT=${NETKIT:-"$HERE/../gfopennetkit"}
TLS=${TLS:-"$HERE/../gfopentls"}
KEYS=${KEYS:-"$HERE/../gfopenkeys"}
OUT=${OUT:-"$HERE/build-amiga"}
CFLAGS="-m68040 -O2 -fomit-frame-pointer -noixemul -Wall -Wextra -Wno-unused-parameter -std=gnu99
        -I$HERE/include -I$NETKIT/include -I$TLS/include"
if [ -f "$KEYS/include/proto/gfopenkeys.h" ]; then
    CFLAGS="$CFLAGS -DHAVE_GFOPENKEYS -I$KEYS/include"
else
    echo "no gfopenkeys headers in $KEYS: building without the keystore (no SASL)"
fi
TLS="$TLS" OUT="$OUT/netkit" sh "$NETKIT/build-amiga.sh"
mkdir -p "$OUT/obj"
OBJS=""
for s in src/parse.c src/client.c src/amiga/irc_amiga.c; do
    o="$OUT/obj/$(basename "$s" .c).o"
    "$CC" $CFLAGS -c "$HERE/$s" -o "$o"
    OBJS="$OBJS $o"
done
"$CC" -m68040 -noixemul -o "$OUT/GFOpenIRC" $OBJS "$OUT/netkit/lib/libgfopennetkit.a"
echo "built $OUT/GFOpenIRC"
