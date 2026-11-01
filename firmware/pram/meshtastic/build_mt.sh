#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the Meshtastic node blob (mt_hdr.S + mt.c + ../selfspi.c) as mt.bin.
# Same patched ARC GCC image as ../build.sh.
set -e
cd "$(dirname "$0")/.."
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  M=meshtastic
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o $M/mt_hdr.o $M/mt_hdr.S &&
  arc-linux-gnu-gcc $CF -c $M/mt.c -o $M/mt.o &&
  arc-linux-gnu-gcc $CF -c selfspi.c -o $M/selfspi.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o $M/mt.elf $M/mt_hdr.o $M/mt.o $M/selfspi.o &&
  arc-linux-gnu-objcopy -O binary $M/mt.elf $M/mt.bin &&
  arc-linux-gnu-objdump -d $M/mt.elf > $M/mt.lst &&
  arc-linux-gnu-size -A $M/mt.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
cd meshtastic
rm -f mt_hdr.o mt.o selfspi.o
echo "mt.bin: $(stat -c %s mt.bin) bytes"
