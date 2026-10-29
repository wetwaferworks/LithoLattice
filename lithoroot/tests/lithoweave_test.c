/* lithoweave_test.c ... host functional test for LithoWeave.
 *
 * Native build and run (this file is a test, not part of the
 * library). Objects are compiled separately because
 * lithoweave_native.c must see the HOST headers only (it plays
 * the host), while everything else sees the LithoRoot headers.
 * LIBC below means the seven LithoLibC objects: src/libc/
 * memory.c, string.c, strings.c, ctype.c, malloc.c, errno.c,
 * strerror.c, each compiled like waferheap.c.
 *
 * Plain:
 *   gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include \
 *       -c src/lithoweave.c -o lw.o
 *   gcc -std=c99 -Wall -Wextra -Werror -c src/lithoweave_native.c \
 *       -o lwn.o
 *   gcc -std=c99 -Wall -Wextra -Werror -I include \
 *       -c src/waferheap.c -o wh.o   (plus LIBC, same flags)
 *   gcc -std=c99 -Wall -Wextra -I include tests/lithoweave_test.c \
 *       lw.o lwn.o wh.o LIBC -o lithoweave_test -pthread \
 *       -Wl,--wrap=malloc,--wrap=free
 *   ./lithoweave_test
 *
 * ASan + UBSan: add -g -fsanitize=address,undefined to every
 * compile and the link, EXCEPT src/libc/malloc.c and
 * src/libc/errno.c, which are compiled plain. Reason: an
 * instrumented interposed malloc is called by the dynamic loader
 * during the sanitizer's own interceptor initialization (the
 * dlsym error path allocates), and its instrumented errno store
 * faults before the sanitizer has registered the executable's
 * globals. The two files are three-line-deep shims; everything
 * substantive (weave, heap, back-end, this test) is instrumented.
 *
 * TSan: add -g -fsanitize=thread to every compile and -no-pie to
 * the link (this host's TSan rejects the PIE layout), EXCEPT the
 * whole malloc path (src/libc/malloc.c, src/libc/errno.c and
 * src/waferheap.c) compiled plain: merely entering an
 * instrumented function on the loader's init path faults TSan
 * the same way, and the heap's accesses are serialized by the
 * injected pthread-mutex hooks, which TSan models. Expect a
 * small number of TSan warnings (2 to 3 observed), all of one
 * class diagnosed in docs/LITHOWEAVE.md: TSan cannot see block
 * ownership transfer through the uninstrumented allocator, so a
 * recycled block's two owner memsets (lw_spawn's control block
 * memset, wh_calloc's internal zeroing) read as a race. Every
 * flagged write is by the block's owner at the time, ordered by
 * the heap mutex; no warning touches weave state (cell, return
 * value, registry, destructor lists). Run TSan under
 * setarch -R on this host: its address layout demands are
 * intermittently refused by the VM's ASLR. Under heavy host load
 * the TSan build can also crawl; the cell polls carry 30 s
 * wall-clock deadlines for that reason.
 *
 * Setup the cases rely on: control blocks come from LithoLibC
 * malloc over a WaferHeap whose arena is mapped below 4 GiB
 * (MAP_32BIT). That is what makes the frozen spawn ABI's
 * uint32_t start_arg lossless on this 64-bit host: the native
 * back-end widens it back to the very pointer lw_spawn narrowed.
 * The heap carries pthread-mutex lock hooks, standing in for the
 * locking LithoThread will inject in production.
 *
 * Leak checks use a linker-wrap oracle: the binary is linked
 * with -Wl,--wrap=malloc,--wrap=free, so allocation calls made
 * by the statically linked objects (the weave, this test) are
 * counted, and the cases assert the count returns exactly to its
 * post-setup value. WaferHeap live-block counts cannot serve as
 * the oracle on this host: with LithoLibC's malloc interposed
 * process-wide, the host libc allocates per-thread bookkeeping
 * through it (stack cache and TLS structures) and retains or
 * releases those blocks as the workload's shape changes...
 * measured both directions, saturating, and identical with raw
 * pthread churn when no weave is linked at all. The wrap counter
 * is immune: calls originating inside the host libc bind to the
 * interposed malloc directly and never pass through the wrapper,
 * so the counter sees the weave's own traffic only (one control
 * block per spawn, one node per destructor registration).
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "lithoweave.h"
#include "waferheap.h"

/* Test hook from the native back-end (src/lithoweave_native.c). */
void lw__native_test_fail_spawn(int count);

static int g_failures = 0;
static wh_heap_t* g_heap = NULL;
static long g_baseline_net = 0;

/* The wrap oracle (see the file header): net wrapped allocations
 * outstanding. __real_malloc / __real_free are LithoLibC's. */
void* __real_malloc(size_t n);
void __real_free(void* p);
static long g_alloc_net = 0;

void* __wrap_malloc(size_t n) {
  void* p = __real_malloc(n);
  if (p != NULL) __atomic_add_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  return p;
}

void __wrap_free(void* p) {
  if (p != NULL) __atomic_sub_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  __real_free(p);
}

static long alloc_net(void) {
  return __atomic_load_n(&g_alloc_net, __ATOMIC_SEQ_CST);
}

/* Poll a tid cell until the host's store of 0 lands, with a
 * wall-clock deadline (30 s: generous because the sanitizer
 * builds schedule threads very slowly; the plain build completes
 * in microseconds). Returns 1 on completion, 0 on deadline. */
static int wait_cell_zero(const uint32_t* cell) {
  struct timespec start, now, nap;
  clock_gettime(CLOCK_MONOTONIC, &start);
  nap.tv_sec = 0;
  nap.tv_nsec = 1000000L; /* 1 ms */
  while (__atomic_load_n((const uint32_t*)cell, __ATOMIC_SEQ_CST) != 0u) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec > 30) return 0;
    nanosleep(&nap, NULL);
  }
  return 1;
}

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      printf("  FAIL check: %s (line %d)\n", #cond, __LINE__);       \
      ++fails;                                                       \
    }                                                                \
  } while (0)

#define CHECK_BALANCED() CHECK(alloc_net() == g_baseline_net)

/* ---- shared state for entries -------------------------------------- */
static uint32_t g_counter;

static void* entry_count(void* arg) {
  uintptr_t idx = (uintptr_t)arg;
  __atomic_add_fetch(&g_counter, 1u, __ATOMIC_SEQ_CST);
  return (void*)(uintptr_t)(1000u + (unsigned)idx);
}

static void* entry_trivial(void* arg) {
  (void)arg;
  return (void*)(uintptr_t)5u;
}

/* ---- case: spawn / join --------------------------------------------- */
static int case_spawn_join(void) {
  int fails = 0;
  enum { N = 16 };
  lw_thread_t hs[N];
  int32_t tids[N];
  uintptr_t sum = 0;
  int i, j;

  __atomic_store_n(&g_counter, 0u, __ATOMIC_SEQ_CST);
  for (i = 0; i < N; ++i) {
    lw_descriptor_t d;
    d.entry = entry_count;
    d.arg = (void*)(uintptr_t)i;
    d.stack_size = 256u * 1024u;
    d.name = "counter";
    CHECK(lw_spawn(&d, &hs[i]) == LITHOSYS_OK);
    tids[i] = lw_thread_tid(hs[i]);
    CHECK(tids[i] > 1);
    /* The spawn ABI accommodation: the block must live below
     * 4 GiB on this host or start_arg could not carry it. */
    CHECK((uintptr_t)lw_thread_cell(hs[i]) < 0x100000000ull);
    for (j = 0; j < i; ++j) CHECK(tids[i] != tids[j]);
  }
  for (i = 0; i < N; ++i) {
    void* rv = NULL;
    CHECK(lw_join(hs[i], &rv) == LITHOSYS_OK);
    sum += (uintptr_t)rv;
  }
  CHECK(sum == (uintptr_t)(N * 1000 + (N * (N - 1)) / 2));
  CHECK(__atomic_load_n(&g_counter, __ATOMIC_SEQ_CST) == (uint32_t)N);
  /* A consumed handle refuses a second join. */
  CHECK(lw_join(hs[0], NULL) == LITHOSYS_EINVAL);
  /* Every control block was reaped by its join. */
  CHECK_BALANCED();
  return fails;
}

/* ---- case: detach, poll the cell ------------------------------------- */
static uint32_t g_detach_stage;

static void* entry_detach(void* arg) {
  (void)arg;
  __atomic_store_n(&g_detach_stage, 77u, __ATOMIC_SEQ_CST);
  return (void*)(uintptr_t)7u;
}

/* Review L7: a stack request the 32-bit spawn word cannot carry is
 * refused, not truncated. */
static int case_stack_too_big(void) {
  int fails = 0;
#if SIZE_MAX > 0xFFFFFFFFu
  lw_descriptor_t d;
  lw_thread_t h;
  d.entry = entry_detach;
  d.arg = NULL;
  d.stack_size = (size_t)0x100000000ull;
  d.name = "too-big";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_EINVAL);
#endif
  return fails;
}

static int case_detach(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h, copy;
  const uint32_t* cell;
  void* rv = NULL;

  __atomic_store_n(&g_detach_stage, 0u, __ATOMIC_SEQ_CST);
  d.entry = entry_detach;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "detached";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  copy = h;
  cell = lw_thread_cell(h);
  CHECK(cell != NULL);
  CHECK(lw_detach(h) == LITHOSYS_OK);
  /* A detached thread refuses join and a second detach. */
  CHECK(lw_join(copy, &rv) == LITHOSYS_EINVAL);
  CHECK(lw_detach(copy) == LITHOSYS_EINVAL);
  /* Poll completion through the cell itself, the protocol word:
   * the host's store of 0 is the completion signal. */
  CHECK(wait_cell_zero(cell));
  CHECK(__atomic_load_n(&g_detach_stage, __ATOMIC_SEQ_CST) == 77u);
  /* The detached block is reaped lazily: a later spawn / join
   * cycle sweeps it. (The cell pointer is not touched past here:
   * the sweep frees the block it points into.) */
  d.entry = entry_trivial;
  d.name = "sweeper";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  CHECK(lw_join(h, &rv) == LITHOSYS_OK);
  CHECK(rv == (void*)(uintptr_t)5u);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: TLS destructors, pass discipline --------------------------- */
static char g_dtor_log[64];
static unsigned g_dtor_len;

static void dtor_log(char c) {
  g_dtor_log[g_dtor_len++] = c;
  g_dtor_log[g_dtor_len] = '\0';
}

static void dtor_c(void* arg) {
  (void)arg;
  dtor_log('c');
}

static void dtor_b(void* arg) {
  (void)arg;
  dtor_log('b');
  /* Registered during pass 1: must run in pass 2, after the rest
   * of pass 1, per the documented pass discipline. */
  lw_thread_atexit(dtor_c, NULL);
}

static void dtor_a(void* arg) {
  (void)arg;
  dtor_log('a');
}

static void* entry_dtors(void* arg) {
  (void)arg;
  lw_thread_atexit(dtor_a, NULL);
  lw_thread_atexit(dtor_b, NULL);
  return NULL;
}

static int case_tls_dtors(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h;

  g_dtor_len = 0;
  g_dtor_log[0] = '\0';
  d.entry = entry_dtors;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "dtors";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  CHECK(lw_join(h, NULL) == LITHOSYS_OK);
  /* Pass 1 drains [b, a] in reverse registration order (b
   * registers c on its way); pass 2 runs [c]. */
  CHECK(g_dtor_len == 3u);
  CHECK(g_dtor_log[0] == 'b' && g_dtor_log[1] == 'a' &&
        g_dtor_log[2] == 'c');
  CHECK_BALANCED();
  return fails;
}

/* ---- case: lw_exit from inside a destructor ---------------------------- */
static void dtor_e(void* arg) {
  (void)arg;
  dtor_log('e');
}

static void dtor_x(void* arg) {
  (void)arg;
  dtor_log('x');
  lw_exit((void*)(uintptr_t)55u); /* abandons the rest of the pass */
}

static void* entry_dtor_exit(void* arg) {
  (void)arg;
  lw_thread_atexit(dtor_e, NULL); /* registered first, runs last */
  lw_thread_atexit(dtor_x, NULL); /* runs second, exits mid-pass */
  lw_thread_atexit(dtor_c, NULL); /* registered last, runs first */
  return (void*)(uintptr_t)7u;
}

static int case_dtor_exit(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h;
  void* rv = NULL;

  g_dtor_len = 0;
  g_dtor_log[0] = '\0';
  d.entry = entry_dtor_exit;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "dtor-exit";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  CHECK(lw_join(h, &rv) == LITHOSYS_OK);
  /* The pass runs c, then x (which exits): e must never run. The
   * exiting thread's value is the entry's 7, not the 55 passed to
   * the mid-destructor lw_exit: the exit was already in progress.
   * The balance check proves e's un-run node was dropped by the
   * landing pad, not leaked. */
  CHECK(g_dtor_len == 2u);
  CHECK(g_dtor_log[0] == 'c' && g_dtor_log[1] == 'x');
  CHECK(rv == (void*)(uintptr_t)7u);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: runaway destructor is capped -------------------------------- */
static unsigned g_runaway_runs;

static void dtor_runaway(void* arg) {
  (void)arg;
  __atomic_add_fetch(&g_runaway_runs, 1u, __ATOMIC_SEQ_CST);
  lw_thread_atexit(dtor_runaway, NULL); /* renews itself forever */
}

static void* entry_runaway(void* arg) {
  (void)arg;
  lw_thread_atexit(dtor_runaway, NULL);
  return NULL;
}

static int case_tls_runaway(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h;

  __atomic_store_n(&g_runaway_runs, 0u, __ATOMIC_SEQ_CST);
  d.entry = entry_runaway;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "runaway";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  /* The join must complete: the cap, not the destructor's
   * cooperation, ends the exit. */
  CHECK(lw_join(h, NULL) == LITHOSYS_OK);
  CHECK(g_runaway_runs == (unsigned)LW_TLS_DTOR_PASS_LIMIT);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: lw_exit unwinds the entry ------------------------------------ */
static uint32_t g_exit_stage;

static void* entry_exit(void* arg) {
  (void)arg;
  __atomic_store_n(&g_exit_stage, 1u, __ATOMIC_SEQ_CST);
  lw_exit((void*)(uintptr_t)42u);
  __atomic_store_n(&g_exit_stage, 2u, __ATOMIC_SEQ_CST); /* unreachable */
  return NULL;
}

static int case_lw_exit(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h;
  void* rv = NULL;

  __atomic_store_n(&g_exit_stage, 0u, __ATOMIC_SEQ_CST);
  d.entry = entry_exit;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "exiter";
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  CHECK(lw_join(h, &rv) == LITHOSYS_OK);
  CHECK(rv == (void*)(uintptr_t)42u);
  CHECK(__atomic_load_n(&g_exit_stage, __ATOMIC_SEQ_CST) == 1u);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: spawn failure leaves nothing behind --------------------------- */
static int case_spawn_failure(void) {
  int fails = 0;
  lw_descriptor_t d;
  lw_thread_t h;
  lw_thread_t sentinel;

  d.entry = entry_trivial;
  d.arg = NULL;
  d.stack_size = 0;
  d.name = "doomed";
  sentinel.control = (lw_control_t*)(uintptr_t)0xDEADu;
  sentinel.tid = -5;
  h = sentinel;
  lw__native_test_fail_spawn(1);
  CHECK(lw_spawn(&d, &h) == LITHOSYS_EAGAIN);
  /* The handle is untouched and the control block was freed. */
  CHECK(h.control == sentinel.control && h.tid == sentinel.tid);
  CHECK_BALANCED();
  /* And the failure was one-shot: the next spawn works. */
  CHECK(lw_spawn(&d, &h) == LITHOSYS_OK);
  CHECK(lw_join(h, NULL) == LITHOSYS_OK);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: self / main thread --------------------------------------------- */
static uint32_t g_main_dtor_ran;

static void dtor_main(void* arg) {
  (void)arg;
  __atomic_store_n(&g_main_dtor_ran, 1u, __ATOMIC_SEQ_CST);
}

static int case_self(void) {
  int fails = 0;
  lw_thread_t self = lw_self();

  CHECK(lw_current_tid() == 1);
  CHECK(lw_thread_tid(self) == 1);
  CHECK(lw_self_cell() != NULL);
  /* A thread cannot join itself. */
  CHECK(lw_join(self, NULL) == LITHOSYS_EINVAL);
  /* Main-thread destructors run through the public runner. */
  __atomic_store_n(&g_main_dtor_ran, 0u, __ATOMIC_SEQ_CST);
  CHECK(lw_thread_atexit(dtor_main, NULL) == LITHOSYS_OK);
  lw_run_tls_destructors();
  CHECK(__atomic_load_n(&g_main_dtor_ran, __ATOMIC_SEQ_CST) == 1u);
  return fails;
}

/* ---- setup + main ---------------------------------------------------------- */
static pthread_mutex_t g_heap_mu = PTHREAD_MUTEX_INITIALIZER;

static void heap_lock(void* user) {
  (void)user;
  pthread_mutex_lock(&g_heap_mu);
}

static void heap_unlock(void* user) {
  (void)user;
  pthread_mutex_unlock(&g_heap_mu);
}

#define RUN_CASE(fn)                                   \
  do {                                                 \
    int f = fn();                                      \
    printf("%s: %s\n", f == 0 ? "PASS" : "FAIL", #fn); \
    g_failures += f;                                   \
  } while (0)

int main(void) {
  const size_t arena_bytes = 16u * 1024u * 1024u;
  void* arena;
  wh_options_t opts;
  lw_descriptor_t d;
  lw_thread_t warm;
  void* rv = NULL;

  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(120); /* a protocol hang must die loudly, not forever */

  arena = mmap(NULL, arena_bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (arena == MAP_FAILED) {
    printf("FAIL: MAP_32BIT arena unavailable\n");
    return 1;
  }
  if ((uintptr_t)arena >= 0x100000000ull) {
    printf("FAIL: arena not below 4 GiB\n");
    return 1;
  }
  opts.grow = NULL;
  opts.grow_user = NULL;
  opts.lock = heap_lock;
  opts.unlock = heap_unlock;
  opts.lock_user = NULL;
  g_heap = wh_heap_create(arena, arena_bytes, &opts);
  if (g_heap == NULL) {
    printf("FAIL: wh_heap_create\n");
    return 1;
  }
  wh_set_default_heap(g_heap);

  if (lw_main_thread_init() != LITHOSYS_OK) {
    printf("FAIL: lw_main_thread_init\n");
    return 1;
  }

  /* Warm-up: run the full concurrency profile once (concurrent
   * spawn / join, a detach polled to completion, a destructor
   * thread) so the host libc's saturating per-thread caches are
   * full before the baseline is taken. */
  {
    enum { WN = 16 };
    lw_thread_t hs[WN];
    const uint32_t* cell;
    int i;
    d.entry = entry_trivial;
    d.arg = NULL;
    d.stack_size = 0;
    d.name = "warmup";
    for (i = 0; i < WN; ++i) {
      if (lw_spawn(&d, &hs[i]) != LITHOSYS_OK) {
        printf("FAIL: warm-up spawn\n");
        return 1;
      }
    }
    for (i = 0; i < WN; ++i) {
      if (lw_join(hs[i], &rv) != LITHOSYS_OK) {
        printf("FAIL: warm-up join\n");
        return 1;
      }
    }
    if (lw_spawn(&d, &warm) != LITHOSYS_OK || lw_detach(warm) != LITHOSYS_OK) {
      printf("FAIL: warm-up detach\n");
      return 1;
    }
    cell = lw_thread_cell(warm);
    if (!wait_cell_zero(cell)) {
      printf("FAIL: warm-up detach never completed\n");
      return 1;
    }
    d.entry = entry_dtors;
    if (lw_spawn(&d, &warm) != LITHOSYS_OK || lw_join(warm, &rv) != LITHOSYS_OK) {
      printf("FAIL: warm-up dtor thread\n");
      return 1;
    }
    g_dtor_len = 0;
    g_dtor_log[0] = '\0';
  }
  printf("setup: heap over MAP_32BIT arena, main thread attached\n");
  g_baseline_net = alloc_net();

  RUN_CASE(case_spawn_join);
  RUN_CASE(case_detach);
  RUN_CASE(case_stack_too_big);
  RUN_CASE(case_tls_dtors);
  RUN_CASE(case_dtor_exit);
  RUN_CASE(case_tls_runaway);
  RUN_CASE(case_lw_exit);
  RUN_CASE(case_spawn_failure);
  RUN_CASE(case_self);

  printf("LITHOWEAVE TEST: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
