#!/bin/bash
# check_sjlj.sh ... first-check (c): compile check_sjlj.c under the
# three flag regimes and print, for each, the undefined symbols the
# object carries. The undefined list is the evidence: it names the
# runtime support each lowering leaves for someone else to provide.
# Usage: bash check_sjlj.sh [llvm-bin-dir]
set -u
cd "$(dirname "$0")" || exit 1
LLVM_BIN=${1:-$HOME/workspace/toolchain/LLVM-20.1.8-Linux-X64/bin}
CC="$LLVM_BIN/clang"
NM="$LLVM_BIN/llvm-nm"
mkdir -p obj-checks

FEATS="-matomics -mbulk-memory"

echo "== A. default flags"
"$CC" --target=wasm32 $FEATS -O1 -c check_sjlj.c -o obj-checks/sjlj_a.o \
  && "$NM" -u obj-checks/sjlj_a.o

echo "== B. -mllvm -wasm-enable-sjlj"
"$CC" --target=wasm32 $FEATS -O1 -mllvm -wasm-enable-sjlj \
  -c check_sjlj.c -o obj-checks/sjlj_b.o \
  && "$NM" -u obj-checks/sjlj_b.o

echo "== C. -mllvm -wasm-enable-sjlj -fwasm-exceptions"
"$CC" --target=wasm32 $FEATS -O1 -mllvm -wasm-enable-sjlj -fwasm-exceptions \
  -c check_sjlj.c -o obj-checks/sjlj_c.o \
  && "$NM" -u obj-checks/sjlj_c.o
