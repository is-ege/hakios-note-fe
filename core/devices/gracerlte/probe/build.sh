#!/bin/sh
# Builds probe-boot.img from the probe and the phone's own (Lineage) BOOT, which supplies the ramdisk and dt.img.
set -eu
cd "$(dirname "$0")"
TEMPLATE=${TEMPLATE:-../../../../../private/20261107T133304Z/boot.img}   # outside the repository
OUT=${OUT:-build}
mkdir -p "$OUT"

CFLAGS="-target aarch64-none-elf -march=armv8-a -mstrict-align -mgeneral-regs-only -ffreestanding -fno-builtin
	-fno-stack-protector -fno-pic -O2 -Wall -Werror"
clang $CFLAGS -c start.S -o "$OUT/start.o"
clang $CFLAGS -c probe.c -o "$OUT/probe.o"
/opt/homebrew/opt/lld/bin/ld.lld -T link.ld -o "$OUT/probe.elf" "$OUT/start.o" "$OUT/probe.o"
/opt/homebrew/opt/lld/bin/ld.lld -T link.ld --oformat=binary -o "$OUT/probe.bin" "$OUT/start.o" "$OUT/probe.o"

python3 ../tools/samsung_bootimg.py --template "$TEMPLATE" --selftest
python3 ../tools/samsung_bootimg.py --template "$TEMPLATE" --kernel "$OUT/probe.bin" --output "$OUT/probe-boot.img"
