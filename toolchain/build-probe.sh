#!/bin/bash
# LithoRoot toolchain piece: probe build, census, and run.
#
# Links every wasm32 library object plus the probe main and the
# EH probe TUs into probe.wasm, censuses its imports, and runs it
# under the Node host stub. The link is the acceptance gate:
# wasm-ld may turn ONLY the 14 frozen lithosys_* functions into
# imports (toolchain/lithosys-allowed.txt); any other undefined
# symbol is a hard link error here, not a silent import.
#
# Usage: bash toolchain/build-probe.sh
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
LLVM=${LK_LLVM_HOME:-$HOME/workspace/toolchain/LLVM-20.1.8-Linux-X64}
CLANG=$LLVM/bin/clang
CLANGXX=$LLVM/bin/clang++
WASM_LD=$LLVM/bin/wasm-ld
FEAT="--target=wasm32 -matomics -mbulk-memory -msign-ext -mthread-model posix"
INC="-I $ROOT/include"

mkdir -p toolchain/obj-probe

$CLANG $FEAT -std=c99 -O1 -Wall -Wextra $INC \
  -c toolchain/probe_main.c -o toolchain/obj-probe/probe_main.o
$CLANGXX $FEAT -std=c++17 -O1 -Wall -Wextra -fwasm-exceptions $INC \
  -c toolchain/check_eh_throw.cpp -o toolchain/obj-probe/check_eh_throw.o
$CLANGXX $FEAT -std=c++17 -O1 -Wall -Wextra -fwasm-exceptions $INC \
  -c toolchain/check_eh_catch.cpp -o toolchain/obj-probe/check_eh_catch.o

$WASM_LD --no-entry \
  --import-memory --shared-memory \
  --initial-memory=16777216 --max-memory=1073741824 \
  --allow-undefined-file=toolchain/lithosys-allowed.txt \
  --export=main --export=lithosys_thread_start --export=malloc \
  --export=lk_probe_tls_block \
  --export-if-defined=__wasm_init_tls --export-if-defined=__wasm_call_ctors \
  toolchain/obj/*.o \
  toolchain/obj-probe/probe_main.o \
  toolchain/obj-probe/check_eh_throw.o \
  toolchain/obj-probe/check_eh_catch.o \
  -o toolchain/probe.wasm

echo "== census =="
node toolchain/census.mjs toolchain/probe.wasm
echo "== run =="
node toolchain/run_probe.mjs toolchain/probe.wasm

# Second link, no GC: every function in the libraries survives,
# so every lithosys reference anywhere in the library code shows
# up in this census. It must still be a subset of the frozen 14.
$WASM_LD --no-entry --no-gc-sections \
  --import-memory --shared-memory \
  --initial-memory=16777216 --max-memory=1073741824 \
  --allow-undefined-file=toolchain/lithosys-allowed.txt \
  --export=main --export=lithosys_thread_start --export=malloc \
  --export=lk_probe_tls_block \
  --export-if-defined=__wasm_init_tls --export-if-defined=__wasm_call_ctors \
  toolchain/obj/*.o \
  toolchain/obj-probe/probe_main.o \
  toolchain/obj-probe/check_eh_throw.o \
  toolchain/obj-probe/check_eh_catch.o \
  -o toolchain/probe_full.wasm

echo "== census (full, no GC) =="
node toolchain/census.mjs toolchain/probe_full.wasm
