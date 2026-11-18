// __cxa_atexit is lock-free now (CAS push). 4 threads register 250 entries each at
// the same time; then __cxa_finalize(nullptr) must run all 1000 exactly once, and
// each thread's entries newest first. Entries for another DSO handle are deferred
// and must survive a finalize of this one. A plain atexit(void(void)) handler
// must run with its own type. rc 0 = pass.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "lithoweave.h"
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" void __cxa_finalize(void*);
static const int T = 4, N = 250;
static int ran[T][N];
static int last_seen[T];
static int order_bad = 0, total = 0, other_ran = 0;
static int dso_a, dso_b;
static void on_exit_fn(void* p) {
  uintptr_t v = (uintptr_t)p; int t = (int)(v >> 16), i = (int)(v & 0xffff);
  ran[t][i] += 1; total += 1;
  if (i >= last_seen[t]) order_bad += 1;  // newest first: indices must fall
  last_seen[t] = i;
}
static void on_other(void*) { other_ran += 1; }
// A plain atexit handler is void(void): it must be called with that type
// (a void(void*) call through the registry traps on wasm).
static int plain_ran = 0;
static void on_plain(void) { plain_ran += 1; }
static void* worker(void* a) {
  int t = (int)(uintptr_t)a;
  for (int i = 0; i < N; ++i) __cxa_atexit(on_exit_fn, (void*)(((uintptr_t)t << 16) | (uintptr_t)i), &dso_a);
  return nullptr;
}
int main() {
  __cxa_atexit(on_other, nullptr, &dso_b);
  if (atexit(on_plain) != 0) return 6;
  lw_thread_t th[T];
  for (int t = 0; t < T; ++t) {
    last_seen[t] = N;
    lw_descriptor_t d = {};
    d.entry = worker; d.arg = (void*)(uintptr_t)t; d.stack_size = 128 * 1024;
    if (lw_spawn(&d, &th[t]) != LITHOSYS_OK) return 2;
  }
  for (int t = 0; t < T; ++t) lw_join(th[t], nullptr);
  __cxa_finalize(&dso_a);
  int missing = 0;
  for (int t = 0; t < T; ++t) for (int i = 0; i < N; ++i) if (ran[t][i] != 1) missing += 1;
  printf("atexit: total %d missing %d order_bad %d other_ran %d\n", total, missing, order_bad, other_ran);
  if (total != T * N || missing || order_bad) return 3;
  if (other_ran != 0) return 4;
  __cxa_finalize(&dso_b);
  if (other_ran != 1) return 5;
  __cxa_finalize(nullptr);  // the atexit() entry (dso NULL)
  printf("atexit: plain handler ran %d\n", plain_ran);
  if (plain_ran != 1) return 7;
  return 0;
}
