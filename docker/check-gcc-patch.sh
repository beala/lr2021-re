#!/bin/sh
# Check docker/gcc-arc-blink.patch against the unpatched compiler: compile GCC 13.3's torture and
# ARC target tests (5,197 files) to assembly with both, with and without -fcall-saved-blink, and
# compare. Expected: identical assembly without the flag; with it, no new compiler errors and
# every function that allocates blink saves it (and restores it if it returns).
# Needs ~45 min the first time (two compiler builds). Work files go to ./gcc-patch-check-work.
set -e
cd "$(dirname "$0")"
docker build -t arc-gcc -f Dockerfile.gcc .
docker build -t arc-gcc-vanilla --build-arg PATCH=0 -f Dockerfile.gcc .
W=$PWD/gcc-patch-check-work; mkdir -p $W; cp gcc-patch-check/compile.sh gcc-patch-check/compare.py $W/
cd $W
[ -d gcc-13.3.0/gcc/testsuite/gcc.target/arc ] || curl -fsSL https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/gcc-13.3.0.tar.xz |
  tar xJ --wildcards 'gcc-13.3.0/gcc/testsuite/gcc.c-torture/*' 'gcc-13.3.0/gcc/testsuite/gcc.target/arc/*' 'gcc-13.3.0/gcc/testsuite/gcc.dg/torture/*'
for img in arc-gcc-vanilla arc-gcc; do
  for cfg in "rf16:-O2 -mcpu=em4_dmips -mrf16" "full:-O2 -mcpu=em4_dmips" \
             "rf16blink:-O2 -mcpu=em4_dmips -mrf16 -fcall-saved-blink" \
             "rf16blinkOs:-Os -mcpu=em4_dmips -mrf16 -fcall-saved-blink" \
             "fullblink:-O2 -mcpu=em4_dmips -fcall-saved-blink"; do
    docker run --rm -u "$(id -u):$(id -g)" -v "$W":/w -w /w $img ./compile.sh out/$img/${cfg%%:*} ${cfg#*:}
  done
done
BASE=arc-gcc-vanilla NEW=arc-gcc python3 compare.py
