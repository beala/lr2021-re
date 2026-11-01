#!/bin/sh
# Build the demodulator blob (demod_hdr.S + demod.c + afsk_rx.h) as demod.bin, a separate image
# from pram.bin. Same patched ARC GCC image as build.sh. The frame output and the vector table
# copy live in PRAM from 0x101A00, so the image must end before that.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o demod_hdr.o demod_hdr.S &&
  arc-linux-gnu-gcc $CF -Wall -c demod.c -o demod.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o demod.elf demod_hdr.o demod.o &&
  arc-linux-gnu-objcopy -O binary demod.elf demod.bin &&
  arc-linux-gnu-objdump -d demod.elf > demod.lst &&
  arc-linux-gnu-size -A demod.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f demod_hdr.o demod.o
n=$(stat -c %s demod.bin)
[ "$n" -le 2560 ] || { echo "demod.bin is $n bytes: overlaps the frame output at 0x101A00"; exit 1; }
echo "demod.bin: $n bytes"
