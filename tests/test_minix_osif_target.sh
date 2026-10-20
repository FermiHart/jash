#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
MINIX_SRC=${MINIX_SRC:?set MINIX_SRC to the MINIX source root}
CC=${CC:-x86_64-linux-gnu-gcc}
READELF=${READELF:-x86_64-linux-gnu-readelf}
NM=${NM:-x86_64-linux-gnu-nm}
OBJDUMP=${OBJDUMP:-x86_64-linux-gnu-objdump}
: "${TMPDIR:?set TMPDIR to an isolated scratch directory}"
TMP=$(mktemp -d "$TMPDIR/jash-minix-osif-target.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
MINIX_BEAR_INCLUDE="$TMP/minix-bear-include"
mkdir -p "$MINIX_BEAR_INCLUDE/minix_surface_api"
cp "$MINIX_SRC/blibc/include/bear/surface.h" \
    "$MINIX_BEAR_INCLUDE/minix_surface_api/surface.h"

GCC_INC=$("$CC" -print-file-name=include)
[ -d "$GCC_INC" ] || { echo "missing compiler builtin headers: $GCC_INC" >&2; exit 2; }
INCLUDES="-I$ROOT/c/osif/minix/include -I$MINIX_BEAR_INCLUDE -I$MINIX_SRC/lib/libc/include -I$MINIX_SRC/include -I$MINIX_SRC/minix/include -I$MINIX_SRC/sys -I$MINIX_SRC/sys/arch/amd64/include -I$MINIX_SRC/common/include -I$MINIX_SRC/minix/include/arch/x86_64/include -I$MINIX_SRC/obj/destdir.amd64/usr/include -I$ROOT/c -I$ROOT/c/osif -I$ROOT/c/osif/minix -include $ROOT/c/osif/minix/pthread.h -isystem $GCC_INC"
CFLAGS="-std=c11 -O2 -ffreestanding -nostdinc -fno-pie -fno-stack-protector -fno-builtin -fcommon -fno-strict-aliasing -m64 -mno-red-zone -mno-mmx -mno-3dnow -mno-avx -mno-avx2 -mno-fma -mno-xsave -mno-xsaveopt -mfpmath=sse -mno-fancy-math-387 -U__linux__ -U__linux -Ulinux -D__minix -DELFSIZE=64 -D_FORTIFY_SOURCE=0 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_NETBSD_SOURCE -D_CTYPE_NOINLINE -DJASH_TARGET_POSIX -DJASH_TARGET_MINIX=1 -Wall -Wextra -Wpedantic -Wno-unused-parameter -Wformat=2 -Wvla -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wno-missing-field-initializers -Werror"

for unit in syscall osinfo poll surface; do
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $INCLUDES -c "$ROOT/c/osif/minix/$unit.c" -o "$TMP/$unit.o"
    "$READELF" -h "$TMP/$unit.o" | grep -Eq 'Class:.*ELF64'
    "$READELF" -h "$TMP/$unit.o" | grep -Eq 'Machine:.*X86-64'
done
# IPC uses the shared POSIX implementation in c/osif/ipc.c.
# shellcheck disable=SC2086
"$CC" $CFLAGS $INCLUDES -c "$ROOT/c/osif/ipc.c" -o "$TMP/ipc.o"
"$READELF" -h "$TMP/ipc.o" | grep -Eq 'Class:.*ELF64'
"$READELF" -h "$TMP/ipc.o" | grep -Eq 'Machine:.*X86-64'
if "$NM" -u "$TMP/syscall.o" | grep -Eq '[[:space:]](__)?syscall$'; then
    echo 'MINIX OSIF syscall object unexpectedly imports a raw syscall entry' >&2
    exit 1
fi
if "$OBJDUMP" -d "$TMP/syscall.o" | grep -Eq '[[:space:]]syscall([[:space:]]|$)'; then
    echo 'MINIX OSIF syscall object contains a raw syscall instruction' >&2
    exit 1
fi
printf 'MINIX OSIF target-header compile PASS (five ELF64 x86_64 objects; host compiler, no target link)\n'
