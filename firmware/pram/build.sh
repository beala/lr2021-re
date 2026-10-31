#!/bin/sh
# Build the PRAM blob: sweep.S (header, entry table, sweep) + fft.c + stage.S, linked at 0x101000.
# Needs the ARC GCC image: docker build -t arc-gcc -f docker/Dockerfile.gcc docker
# Output: pram.bin (+ pram.lst). The image must end before the sweep's result buffer (0x101C00).
set -e
cd "$(dirname "$0")"
python3 gen_tables.py
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o sweep.o sweep.S &&
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o stage.o stage.S &&
  arc-linux-gnu-gcc $CF -c fft.c -o fft.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o pram.elf sweep.o fft.o stage.o &&
  arc-linux-gnu-objcopy -O binary pram.elf pram.bin &&
  arc-linux-gnu-objdump -d pram.elf > pram.lst &&
  arc-linux-gnu-size -A pram.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty: initialise those variables\"; exit 1 } }"'
rm -f sweep.o fft.o stage.o
size=$(stat -c %s pram.bin)
echo "pram.bin: $size bytes"
if [ "$size" -gt $((0x1C00 - 0x1000)) ]; then echo "image overlaps the sweep buffer at 0x101C00" >&2; exit 1; fi
