#!/bin/sh
# Build the waterfall text blob (text_hdr.S + text.c) as text.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o text_hdr.o text_hdr.S &&
  arc-linux-gnu-gcc $CF -c text.c -o text.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o text.elf text_hdr.o text.o &&
  arc-linux-gnu-objcopy -O binary text.elf text.bin &&
  arc-linux-gnu-objdump -d text.elf > text.lst &&
  arc-linux-gnu-size -A text.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f text_hdr.o text.o
echo "text.bin: $(stat -c %s text.bin) bytes"
