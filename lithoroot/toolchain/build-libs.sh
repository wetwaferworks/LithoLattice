#!/bin/bash
# build-libs.sh ... compile every LithoRoot translation unit for
# wasm32 with the stock clang, into lithoroot/toolchain/obj/.
# This is the toolchain piece's per-piece compile matrix; see
# docs/TOOLCHAIN.md for the results table and the reasoning for
# each flag. Re-runnable: wipes and recreates obj/.
#
# Usage: bash build-libs.sh [llvm-bin-dir]
#   llvm-bin-dir defaults to ~/workspace/toolchain/LLVM-20.1.8-Linux-X64/bin
set -u
cd "$(dirname "$0")/.." || exit 1   # -> lithoroot/
ROOT=$PWD
LLVM_BIN=${1:-$HOME/workspace/toolchain/LLVM-20.1.8-Linux-X64/bin}
CC="$LLVM_BIN/clang"
CXX="$LLVM_BIN/clang++"
OBJ=toolchain/obj
rm -rf "$OBJ"
mkdir -p "$OBJ"

FEATS="-matomics -mbulk-memory -msign-ext -mthread-model posix"
# -ffreestanding -fno-builtin (review L8): LithoLibC implements memcpy,
# memmove, memset and friends itself, so the compiler must not
# recognise their loops as those builtins and lower them back into
# self-calls (not observed at -O1/-O2 on clang 20, but nothing else
# prevents it under LTO or a later LLVM).
# -DLITHOROOT_WITH_THREAD (review L9): this matrix links LithoThread,
# whose lithothread_errno.c owns __litholibc_errno_location; errno.c
# compiles empty under the define even if it is ever added below.
CFLAGS="--target=wasm32 -std=c99 -O1 -Wall -Wextra -ffreestanding -fno-builtin -DLITHOROOT_WITH_THREAD $FEATS -I include"
CXXFLAGS="--target=wasm32 -std=c++17 -O1 -Wall -Wextra $FEATS -fwasm-exceptions -I include"

# The wasm sysroot translation units. Excluded on purpose:
#   src/libc/errno.c        the LithoThread link rule: a link with
#                           LithoThread takes __litholibc_errno_location
#                           from src/lithothread_errno.c instead
#   src/*_native.c, src/lithoabi/backend_native_scaffold.cc
#                           host test scaffolding, never wasm code
#   src/lithosys_volume_native.c, tests/* : host doubles and gates
C_TUS="
src/waferheap.c
src/libc/bigint.c
src/libc/ctype.c
src/libc/dtoa.c
src/libc/malloc.c
src/libc/math.c
src/libc/memory.c
src/libc/printf.c
src/libc/scanf.c
src/libc/setjmp.c
src/libc/stdio.c
src/libc/stdlib.c
src/libc/strerror.c
src/libc/string.c
src/libc/strings.c
src/libc/strtod.c
src/libc/time.c
src/lithoweave.c
src/lithothread.c
src/lithothread_errno.c
src/lithothread_heap.c
src/rt/int128.c
src/crt/crt1.c
"

# Per-TU extra flags (see docs/TOOLCHAIN.md, first-check (c)):
#   setjmp.c carries __builtin_wasm_throw for __wasm_longjmp, which
#     needs full wasm exceptions in that TU.
#   lithoweave.c / lithothread.c are the two setjmp consumers; the
#     SjLj lowering rewrites their setjmp / longjmp calls.
extra_flags_for() {
  case "$1" in
    src/libc/setjmp.c)  echo "-fwasm-exceptions" ;;
    src/lithoweave.c)   echo "-mllvm -wasm-enable-sjlj" ;;
    src/lithothread.c)  echo "-mllvm -wasm-enable-sjlj" ;;
    *)                  echo "" ;;
  esac
}
CXX_TUS="
src/libc/new_delete.cc
src/lithoabi/atexit.cc
src/lithoabi/backend_wasm.cc
src/lithoabi/eh_runtime.cc
src/lithoabi/exception.cc
src/lithoabi/guard.cc
src/lithoabi/personality.cc
src/lithoabi/std_exceptions.cc
src/lithoabi/stubs.cc
src/lithoabi/typeinfo.cc
"

fail=0
for tu in $C_TUS; do
  out="$OBJ/$(echo "$tu" | tr / _).o"
  extra=$(extra_flags_for "$tu")
  if "$CC" $CFLAGS $extra -c "$tu" -o "$out" 2> "$out.log"; then
    echo "OK   $tu"
  else
    echo "FAIL $tu (see $out.log)"; fail=1
  fi
done
for tu in $CXX_TUS; do
  out="$OBJ/$(echo "$tu" | tr / _).o"
  if "$CXX" $CXXFLAGS -c "$tu" -o "$out" 2> "$out.log"; then
    echo "OK   $tu"
  else
    echo "FAIL $tu (see $out.log)"; fail=1
  fi
done
exit $fail
