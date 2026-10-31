#!/bin/sh
# compile.sh OUTDIR FLAGS... : runs inside an ARC GCC image; compiles every C file of the GCC
# torture tests and the ARC target tests to assembly, recording exit code and stderr per file.
out=$1; shift; mkdir -p $out
export FLAGS="$*" OUT=$out
T=gcc-13.3.0/gcc/testsuite
find $T/gcc.c-torture/execute $T/gcc.c-torture/compile $T/gcc.dg/torture $T/gcc.target/arc -maxdepth 1 -name '*.c' | sort |
xargs -P "$(nproc)" -n 1 sh -c '
  f=$0; n=$(echo $f | sed "s|gcc-13.3.0/gcc/testsuite/||; s|/|__|g")
  timeout 60 arc-linux-gnu-gcc $FLAGS -w -fno-ident -S -o $OUT/$n.s $f 2> $OUT/$n.err; echo $? > $OUT/$n.rc'
