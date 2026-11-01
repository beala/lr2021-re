#!/bin/sh
# Build the MeshCore repeater blob (mesh_hdr.S + mesh.c + selfspi.c) as mesh.bin.
# Same patched ARC GCC image as build.sh.
set -e
cd "$(dirname "$0")"
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-gcc sh -c '
  CF="-mcpu=em4_dmips -mrf16 -mno-sdata -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-pic -fcall-saved-blink"
  arc-linux-gnu-gcc -mcpu=em4_dmips -mrf16 -c -o mesh_hdr.o mesh_hdr.S &&
  arc-linux-gnu-gcc $CF -c mesh.c -o mesh.o &&
  arc-linux-gnu-gcc $CF -c selfspi.c -o selfspi.o &&
  arc-linux-gnu-ld -EL -T pram.ld -o mesh.elf mesh_hdr.o mesh.o selfspi.o &&
  arc-linux-gnu-objcopy -O binary mesh.elf mesh.bin &&
  arc-linux-gnu-objdump -d mesh.elf > mesh.lst &&
  arc-linux-gnu-size -A mesh.elf | awk "/^.bss/ { if (\$2 != 0) { print \".bss is not empty\"; exit 1 } }"'
rm -f mesh_hdr.o mesh.o selfspi.o
echo "mesh.bin: $(stat -c %s mesh.bin) bytes"
