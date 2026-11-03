#!/bin/sh
# Build the raycaster blob (ray_hdr.S + ray.c) as ray.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o ray_hdr.o ray_hdr.S &&
  arc-linux-gnu-gcc $CF -c ray.c -o ray.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o ray.elf ray_hdr.o ray.o &&
  arc-linux-gnu-objcopy -O binary ray.elf ray.bin &&
  arc-linux-gnu-objdump -d ray.elf > ray.lst &&
  arc-linux-gnu-size -A ray.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f ray_hdr.o ray.o
echo "ray.bin: $(stat -c %s ray.bin) bytes"
