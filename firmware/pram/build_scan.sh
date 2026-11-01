#!/bin/sh
# Build the scanner blob (scan_hdr.S + scan.c) as scan.bin, a separate image from pram.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o scan_hdr.o scan_hdr.S &&
  arc-linux-gnu-gcc $CF -c scan.c -o scan.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o scan.elf scan_hdr.o scan.o &&
  arc-linux-gnu-objcopy -O binary scan.elf scan.bin &&
  arc-linux-gnu-objdump -d scan.elf > scan.lst &&
  arc-linux-gnu-size -A scan.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f scan_hdr.o scan.o
echo "scan.bin: $(stat -c %s scan.bin) bytes"
