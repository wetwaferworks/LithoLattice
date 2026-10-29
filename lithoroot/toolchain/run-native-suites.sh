#!/bin/bash
# run-native-suites.sh ... build and run the LithoRoot native (host)
# test suites, following the build recipe in each test file's
# header comment. Linux/WSL with gcc/g++/objcopy required.
#
#   toolchain/run-native-suites.sh [suite...]
#   suites: waferheap libc_core lithoweave lithothread libc_stdio
#           lithoabi libc_new_delete wasm_guard   (default: all)
#
# Output objects go to $LR_OUT (default /tmp/lr-native-suites).
# Exit status is non-zero if any selected suite fails to build or
# run. wasm_guard is skipped (exit 0, "SKIP") when no clang with a
# wasm32 target is found ($LR_WASM_CLANG overrides the search; $LR_WASM_TARGET
# overrides the target flag, e.g. "-target wasm32-freestanding" for
# zig c++).
set -u
cd "$(dirname "$0")/.."
OUT=${LR_OUT:-/tmp/lr-native-suites}
mkdir -p "$OUT"
CW="-std=c99 -pedantic -Wall -Wextra"
CXW="-std=c++17 -fno-exceptions -Wall -Wextra"
PT="create join detach self equal exit attr_init attr_destroy
attr_setdetachstate attr_getdetachstate attr_setstacksize attr_getstacksize
mutex_init mutex_destroy mutex_lock mutex_trylock mutex_unlock
mutex_timedlock mutexattr_init mutexattr_destroy mutexattr_settype
mutexattr_gettype mutexattr_setpshared mutexattr_getpshared cond_init
cond_destroy cond_wait cond_timedwait cond_signal cond_broadcast
condattr_init condattr_destroy condattr_setclock condattr_getclock
condattr_setpshared condattr_getpshared rwlock_init rwlock_destroy
rwlock_rdlock rwlock_tryrdlock rwlock_wrlock rwlock_trywrlock
rwlock_unlock rwlock_timedrdlock rwlock_timedwrlock rwlockattr_init
rwlockattr_destroy rwlockattr_setpshared rwlockattr_getpshared once
key_create key_delete setspecific getspecific"
REN=""; for n in $PT; do REN="$REN -Dpthread_$n=lt_pthread_$n"; done
LIBC6="memory string strings ctype malloc strerror"

cc_() { gcc "$@" || exit 1; }

s_waferheap() { local d=$OUT/wh; mkdir -p $d
  cc_ -std=c99 -Wall -Wextra -I include tests/waferheap_test.c src/waferheap.c -o $d/t
  $d/t; }

s_libc_core() { local d=$OUT/core; mkdir -p $d; local L=""
  for f in $LIBC6 errno; do L="$L src/libc/$f.c"; done
  cc_ -std=c99 -Wall -Wextra -I include tests/libc_core_test.c $L src/waferheap.c -o $d/t -ldl
  $d/t; }

s_lithoweave() { local d=$OUT/lw; mkdir -p $d
  cc_ $CW -Werror -I include -c src/lithoweave.c -o $d/lw.o
  cc_ -std=c99 -Wall -Wextra -Werror -c src/lithoweave_native.c -o $d/lwn.o
  cc_ $CW -Werror -I include -c src/waferheap.c -o $d/wh.o
  for f in $LIBC6 errno; do cc_ $CW -Werror -I include -c src/libc/$f.c -o $d/libc_$f.o; done
  cc_ -std=c99 -Wall -Wextra -I include tests/lithoweave_test.c $d/*.o -o $d/t -pthread \
      -Wl,--wrap=malloc,--wrap=free
  $d/t; }

thread_objs() { local d=$1; shift
  for f in lithothread lithothread_errno lithothread_heap; do
    cc_ $CW -I include $REN -c src/$f.c -o $d/$f.o; done
  for f in src/lithoweave.c src/waferheap.c; do
    cc_ $CW -I include -c $f -o $d/$(basename $f .c).o; done
  for f in $LIBC6 "$@"; do cc_ $CW -I include -c src/libc/$f.c -o $d/libc_$f.o; done
  cc_ -std=c99 -Wall -Wextra -c src/lithoweave_native.c -o $d/lithoweave_native.o
  cc_ -std=c99 -Wall -Wextra -c src/lithothread_native.c -o $d/lithothread_native.o; }

s_lithothread() { local d=$OUT/lt; rm -rf $d; mkdir -p $d
  thread_objs $d
  cc_ $CW -Werror -I include $REN -c tests/lithothread_test.c -o $d/lithothread_test.o
  cc_ $d/*.o -o $d/t -pthread -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
  $d/t; }

STDIO_LIBC="bigint dtoa strtod stdlib math time printf scanf"
s_libc_stdio() { local d=$OUT/stdio; rm -rf $d; mkdir -p $d
  thread_objs $d $STDIO_LIBC
  cc_ $CW -I include $REN -c src/libc/stdio.c -o $d/stdio.o
  for f in src/lithosys_volume_native.c tests/lithosys_host_double.c tests/libc_stdio_test.c; do
    cc_ $CW -I include -c $f -o $d/$(basename $f .c).o; done
  objcopy --redefine-sym exit=lk_host_exit --redefine-sym abort=lk_host_abort $d/lithoweave_native.o || exit 1
  cc_ $d/*.o -o $d/t -ldl -pthread
  $d/t; }

abi_objs() { local d=$1
  for f in eh_runtime std_exceptions stubs typeinfo personality backend_native_scaffold; do
    g++ $CXW -I include -I src/lithoabi -c src/lithoabi/$f.cc -o $d/$f.o || exit 1; done
  for f in exception atexit guard; do
    g++ $CXW -I include -I src/lithoabi $REN -c src/lithoabi/$f.cc -o $d/$f.o || exit 1; done; }

s_lithoabi() { local d=$OUT/abi; rm -rf $d; mkdir -p $d
  thread_objs $d; abi_objs $d
  g++ $CXW -I include -I src/lithoabi $REN tests/lithoabi_test.cpp $d/*.o \
      -o $d/t -nostdlib++ -pthread || exit 1
  $d/t; }

s_libc_new_delete() { local d=$OUT/nd; rm -rf $d; mkdir -p $d
  thread_objs $d $STDIO_LIBC
  cc_ $CW -I include $REN -c src/libc/stdio.c -o $d/stdio.o
  for f in src/lithosys_volume_native.c tests/lithosys_host_double.c; do
    cc_ $CW -I include -c $f -o $d/$(basename $f .c).o; done
  objcopy --redefine-sym exit=lk_host_exit --redefine-sym abort=lk_host_abort $d/lithoweave_native.o || exit 1
  abi_objs $d
  g++ $CXW -I include -c src/libc/new_delete.cc -o $d/new_delete.o || exit 1
  g++ $CXW -I include tests/libc_new_delete_test.cpp $d/*.o -o $d/t -nostdlib++ -ldl -pthread || exit 1
  $d/t; }

# C1 regression on the real target: compile a function-local static
# for wasm32 with the LithoAbi headers and check that the guard the
# compiler emits is 4 bytes and that guard.cc's accessors use the
# same width (tests/wasm_guard_check.cpp holds the static_asserts).
s_wasm_guard() { local cl=${LR_WASM_CLANG:-}
  if [ -z "$cl" ]; then for c in clang++ clang++-20 clang++-19 clang++-18; do
    command -v $c >/dev/null && { cl=$c; break; }; done; fi
  if [ -z "$cl" ]; then echo "wasm_guard: SKIP (no clang; set LR_WASM_CLANG)"; return 0; fi
  local d=$OUT/wg; mkdir -p $d
  local tg=${LR_WASM_TARGET:---target=wasm32}
  $cl $tg -matomics -mbulk-memory -pthread -fthreadsafe-statics -fno-exceptions \
      -std=c++17 -nostdlib -I include -I src/lithoabi -S -emit-llvm tests/wasm_guard_check.cpp -o $d/g.ll || exit 1
  if grep -E '@_ZGV.*= internal global i32 0, align 4' $d/g.ll >/dev/null; then
    echo "wasm_guard: guard is i32 align 4 on wasm32"; else echo "wasm_guard: FAIL"; grep _ZGV $d/g.ll; return 1; fi
  $cl $tg -matomics -mbulk-memory -pthread -fno-exceptions -std=c++17 -nostdlib \
      -I include -I src/lithoabi -c src/lithoabi/guard.cc -o $d/guard.o || exit 1
  echo "wasm_guard: guard.cc compiles for wasm32: PASS"; }

ALL="waferheap libc_core lithoweave lithothread libc_stdio lithoabi libc_new_delete wasm_guard"
[ $# -eq 0 ] && set -- $ALL
rc=0
for s in "$@"; do
  echo "######## $s"
  ( s_$s ) ; r=$?
  if [ $r -eq 0 ]; then echo "######## $s: OK"; else echo "######## $s: FAILED ($r)"; rc=1; fi
done
exit $rc