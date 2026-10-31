#!/bin/sh
# Assemble a PRAM blob with the ARC binutils image (docker/Dockerfile: docker build -t arc-binutils docker)
# usage: ./build.sh [name]   (default: sweep) -> <name>.bin + <name>.lst
set -e
cd "$(dirname "$0")"
n=${1:-sweep}
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/w -w /w arc-binutils sh -c "
  arc-linux-gnu-as -mcpu=em4_dmips -o $n.o $n.S &&
  arc-linux-gnu-ld -EL -Ttext=0x101000 -e _start -o $n.elf $n.o &&
  arc-linux-gnu-objcopy -O binary $n.elf $n.bin &&
  arc-linux-gnu-objdump -d $n.elf > $n.lst"
rm -f $n.o
ls -l $n.bin
