#!/bin/sh
# Build the direct-PLL blob (pll_hdr.S + pll.c) as pll.bin, a separate image from pram.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o pll_hdr.o pll_hdr.S &&
  arc-linux-gnu-gcc $CF -c pll.c -o pll.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o pll.elf pll_hdr.o pll.o &&
  arc-linux-gnu-objcopy -O binary pll.elf pll.bin &&
  arc-linux-gnu-objdump -d pll.elf > pll.lst &&
  arc-linux-gnu-size -A pll.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f pll_hdr.o pll.o
echo "pll.bin: $(stat -c %s pll.bin) bytes"
