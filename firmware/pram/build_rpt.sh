#!/bin/sh
# Build the repeater blob (rpt_hdr.S + rpt.c) as rpt.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o rpt_hdr.o rpt_hdr.S &&
  arc-linux-gnu-gcc $CF -c rpt.c -o rpt.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o rpt.elf rpt_hdr.o rpt.o &&
  arc-linux-gnu-objcopy -O binary rpt.elf rpt.bin &&
  arc-linux-gnu-objdump -d rpt.elf > rpt.lst &&
  arc-linux-gnu-size -A rpt.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f rpt_hdr.o rpt.o
echo "rpt.bin: $(stat -c %s rpt.bin) bytes"
