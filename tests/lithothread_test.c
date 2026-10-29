/* lithothread_test.c ... host functional test for LithoThread.
 *
 * THE SYMBOL PREFIX (read this first). This test links the
 * LithoThread sources under lt_ prefixed public symbols: every
 * pthread_* function name in the rename list below is compiled
 * as lt_pthread_* in lithothread.c, lithothread_errno.c,
 * lithothread_heap.c and this file. Reason: the two native
 * back-ends (lithoweave_native.c, lithothread_native.c) are
 * statically linked into the same executable and call the HOST's
 * pthread_create / pthread_mutex_* / pthread_cond_* for their
 * registries. If the library linked under its real names, the
 * executable's own definitions would interpose those calls and
 * the back-end's pthread_create would recurse into ours. Under
 * the prefix, host pthread stays host pthread everywhere, and
 * the code under test is byte-for-byte the production code with
 * only its symbol names changed (the same recipe Piece 3 used
 * for its sanitizer runs, extended to every run here). This file
 * therefore calls lt_pthread_* explicitly.
 *
 * The other interposition in the room is deliberate and shared
 * with Pieces 3 and 4: LithoLibC's malloc family IS the process
 * malloc in this link (src/libc/errno.c is NOT linked: the
 * LithoThread link rule puts lithothread_errno.c in its place),
 * and the leak oracle is the linker wrap counter over malloc /
 * free, exactly as in the weave test. Objects:
 *
 *   RENAMED (compiled with the -D list + -I include):
 *     src/lithothread.c, src/lithothread_errno.c,
 *     src/lithothread_heap.c, tests/lithothread_test.c
 *   PLAIN (-I include): src/lithoweave.c, src/waferheap.c,
 *     src/libc/{memory,string,strings,ctype,malloc,strerror}.c
 *   HOST-ONLY (no -I include): src/lithoweave_native.c,
 *     src/lithothread_native.c
 *
 * Plain build (from ~/workspace/lk/lithoroot):
 *   NAMES="pthread_create pthread_join pthread_detach pthread_self
 *     pthread_equal pthread_exit pthread_attr_init pthread_attr_destroy
 *     pthread_attr_setdetachstate pthread_attr_getdetachstate
 *     pthread_attr_setstacksize pthread_attr_getstacksize
 *     pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock
 *     pthread_mutex_trylock pthread_mutex_unlock
 *     pthread_mutex_timedlock pthread_mutexattr_init
 *     pthread_mutexattr_destroy pthread_mutexattr_settype
 *     pthread_mutexattr_gettype pthread_mutexattr_setpshared
 *     pthread_mutexattr_getpshared pthread_cond_init
 *     pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait
 *     pthread_cond_signal pthread_cond_broadcast
 *     pthread_condattr_init pthread_condattr_destroy
 *     pthread_condattr_setclock pthread_condattr_getclock
 *     pthread_condattr_setpshared pthread_condattr_getpshared
 *     pthread_rwlock_init pthread_rwlock_destroy
 *     pthread_rwlock_rdlock pthread_rwlock_tryrdlock
 *     pthread_rwlock_wrlock pthread_rwlock_trywrlock
 *     pthread_rwlock_unlock pthread_rwlock_timedrdlock
 *     pthread_rwlock_timedwrlock pthread_rwlockattr_init
 *     pthread_rwlockattr_destroy pthread_rwlockattr_setpshared
 *     pthread_rwlockattr_getpshared pthread_once pthread_key_create
 *     pthread_key_delete pthread_setspecific pthread_getspecific"
 *   REN=""; for n in $NAMES; do REN="$REN -D$n=lt_$n"; done
 *   for f in lithothread lithothread_errno lithothread_heap; do
 *     gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include $REN \
 *         -c src/$f.c -o $f.o; done
 *   gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include $REN \
 *       -c tests/lithothread_test.c -o lithothread_test.o
 *   for f in src/lithoweave.c src/waferheap.c src/libc/memory.c \
 *            src/libc/string.c src/libc/strings.c src/libc/ctype.c \
 *            src/libc/malloc.c src/libc/strerror.c; do
 *     gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include \
 *         -c $f -o $(basename $f .c).o; done
 *   gcc -std=c99 -Wall -Wextra -Werror -c src/lithoweave_native.c \
 *       -o lithoweave_native.o
 *   gcc -std=c99 -Wall -Wextra -Werror -c src/lithothread_native.c \
 *       -o lithothread_native.o
 *   gcc *.o -o lithothread_test -pthread \
 *       -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
 *   ./lithothread_test
 *
 * ASan + UBSan: as the plain build but add -g
 * -fsanitize=address,undefined to every compile and the link,
 * EXCEPT src/libc/malloc.c and src/lithothread_errno.c compiled
 * plain: both sit on the loader / sanitizer-init path (the
 * interposed malloc, and the errno accessor its failure paths
 * call), the arrangement Pieces 3 and 4 documented.
 *
 * TSan: as the plain build but add -g -fsanitize=thread to every
 * compile and -no-pie to the link, EXCEPT the whole malloc path
 * (src/libc/malloc.c, src/waferheap.c, src/lithothread_errno.c)
 * compiled plain, define LT_NO_HOOK_DEFAULT_HEAP for the test
 * translation unit, and run under setarch -R on this host. The
 * define exists because of a diagnosed interaction, not a race
 * in the code under test: glibc calls the (interposed) free from
 * ld.so's TLS teardown on an exiting thread, and with the glue
 * lock on the default heap that path enters an instrumented
 * mutex while TSan is recycling the dying thread's trace, which
 * faults inside TSan's own runtime. The plain and ASan builds
 * run the hooked default heap in full. Run with
 * TSAN_OPTIONS=halt_on_error=0 (this toolchain's TSan otherwise
 * stops the process at an early warning): the suite then runs
 * to completion and the warnings are the one class diagnosed in
 * docs/LITHOTHREAD.md, cross-lifetime pairs on recycled heap
 * addresses whose free/realloc middle lives in the
 * uninstrumented allocator (10 warnings in the landing run, all
 * summarized at the calloc memset store; none on synchronization
 * state or user data).
 *
 * Setup mirrors the weave test: the default heap is a WaferHeap
 * over a MAP_32BIT arena (the weave's frozen spawn ABI narrows
 * control block pointers to uint32_t), built with
 * lithothread_heap_options() so the glue under test guards the
 * default heap from the first spawn.
 */

#define _GNU_SOURCE

/* Host <time.h> first (audit fix run, 2026-10-28; the D-F5
 * pattern from tests/lithoabi_test.cpp): this file uses the
 * host's clock_gettime / nanosleep, and since the LithoLibC
 * stdio piece landed its own include/time.h (which declares
 * neither, by census), the recipe's -I include shadows the
 * host header and this file stopped compiling. Defining the
 * LithoLibC header's guard and including the host header by
 * path restores the arrangement the file was written against. */
#define LITHOLIBC_TIME_H
#include </usr/include/time.h>

#include <pthread.h> /* Resolves to LithoRoot's own header under
                        the build recipe (-I include first); the
                        calls below are the lt_ prefixed ones.
                        Host facilities come from stdio / time /
                        unistd / sys/mman. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "lithoweave.h"
#include "waferheap.h"

/* Wiring calls (lithothread_internal.h by declaration, the same
 * way the weave test declares its back-end hook). */
void __litholibc_errno_arm_tls(void);
wh_options_t lithothread_heap_options(void);
int* __litholibc_errno_location(void);
#define LT_ERRNO (*__litholibc_errno_location())

#include <litholibc/errno_values.h>

static int g_failures = 0;
static long g_baseline_net = 0;

/* The wrap oracle (see the file header and the weave test).
 * Extended over Piece 4's malloc/free pair: LithoThread allocates
 * its records with calloc, so calloc and realloc are wrapped too
 * and every counted entry point has its counted exit. A realloc
 * that keeps or moves a live block is net zero; realloc(NULL, n)
 * counts as a fresh allocation, realloc(p, 0) as a free (the
 * LithoLibC semantics). */
void* __real_malloc(size_t n);
void* __real_calloc(size_t n, size_t sz);
void* __real_realloc(void* p, size_t n);
void __real_free(void* p);
static long g_alloc_net = 0;

void* __wrap_malloc(size_t n) {
  void* p = __real_malloc(n);
  if (p != NULL) __atomic_add_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  return p;
}

void* __wrap_calloc(size_t n, size_t sz) {
  void* p = __real_calloc(n, sz);
  if (p != NULL) __atomic_add_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  return p;
}

void* __wrap_realloc(void* p, size_t n) {
  void* q = __real_realloc(p, n);
  if (p == NULL && q != NULL) {
    __atomic_add_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  } else if (n == 0 && p != NULL) {
    __atomic_sub_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  }
  return q;
}

void __wrap_free(void* p) {
  if (p != NULL) __atomic_sub_fetch(&g_alloc_net, 1L, __ATOMIC_SEQ_CST);
  __real_free(p);
}

static long alloc_net(void) {
  return __atomic_load_n(&g_alloc_net, __ATOMIC_SEQ_CST);
}

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      printf("  FAIL check: %s (line %d)\n", #cond, __LINE__);       \
      ++fails;                                                       \
    }                                                                \
  } while (0)

#define CHECK_BALANCED() CHECK(alloc_net() == g_baseline_net)

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void nap_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/* Poll an atomic flag to 1 with a deadline (seconds are generous:
 * sanitizer builds schedule slowly). Returns 1 on success. */
static int wait_flag(volatile uint32_t* flag, int seconds) {
  uint64_t deadline = now_ms() + (uint64_t)seconds * 1000ull;
  while (__atomic_load_n((const uint32_t*)flag, __ATOMIC_SEQ_CST) == 0u) {
    if (now_ms() > deadline) return 0;
    nap_ms(1);
  }
  return 1;
}

/* ---- case: create / join ------------------------------------------- */
static void* entry_index(void* arg) {
  return (void*)(uintptr_t)(5000u + (uintptr_t)arg);
}

static int case_create_join(void) {
  int fails = 0;
  enum { N = 8 };
  pthread_t th[N];
  int i;

  for (i = 0; i < N; ++i) {
    CHECK(lt_pthread_create(&th[i], NULL, entry_index,
                            (void*)(uintptr_t)i) == 0);
  }
  for (i = 0; i < N; ++i) {
    void* rv = NULL;
    CHECK(lt_pthread_join(th[i], &rv) == 0);
    CHECK(rv == (void*)(uintptr_t)(5000u + (unsigned)i));
  }
  /* self / equal / double join / self join */
  CHECK(lt_pthread_equal(lt_pthread_self(), lt_pthread_self()) != 0);
  CHECK(lt_pthread_equal(th[0], th[1]) == 0);
  CHECK(lt_pthread_join(th[0], NULL) == ESRCH); /* consumed handle */
  CHECK(lt_pthread_join(lt_pthread_self(), NULL) == EDEADLK);
  CHECK(lt_pthread_create(NULL, NULL, entry_index, NULL) == EINVAL);
  CHECK_BALANCED();
  return fails;
}

/* ---- case: detach ----------------------------------------------------- */
static volatile uint32_t g_detach_done;

static void* entry_setflag(void* arg) {
  (void)arg;
  __atomic_store_n(&g_detach_done, 1u, __ATOMIC_SEQ_CST);
  return NULL;
}

/* Reclamation of a detached thread is lazy in BOTH layers (the
 * weave sweeps its control block at the next spawn; LithoThread
 * reaps its record at the next create), and the detached thread's
 * completion lands asynchronously after its last user store. So
 * run spawn/join sweeper cycles until the balance converges; a
 * genuine leak never converges. */
static int settle_balance(int max_cycles) {
  int i;
  for (i = 0; i < max_cycles; ++i) {
    pthread_t th;
    if (alloc_net() == g_baseline_net) return 1;
    if (lt_pthread_create(&th, NULL, entry_index, (void*)(uintptr_t)3) != 0) {
      return 0;
    }
    if (lt_pthread_join(th, NULL) != 0) return 0;
  }
  return alloc_net() == g_baseline_net;
}

static int case_detach(void) {
  int fails = 0;
  pthread_t th;
  pthread_attr_t attr;

  __atomic_store_n(&g_detach_done, 0u, __ATOMIC_SEQ_CST);
  CHECK(lt_pthread_attr_init(&attr) == 0);
  CHECK(lt_pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
  CHECK(lt_pthread_create(&th, &attr, entry_setflag, NULL) == 0);
  CHECK(lt_pthread_attr_destroy(&attr) == 0);
  CHECK(wait_flag(&g_detach_done, 30));
  CHECK(settle_balance(50));
  CHECK_BALANCED();

  /* Detach a joinable thread after creation. */
  CHECK(lt_pthread_create(&th, NULL, entry_setflag, NULL) == 0);
  CHECK(lt_pthread_detach(th) == 0);
  /* The second detach and the join race the detached thread's
   * own reclamation: if its record still lives the answer is
   * EINVAL; if the exit path already reaped it, ESRCH. POSIX
   * leaves detaching a detached thread undefined; either answer
   * proves the operation was refused. (Slow sanitizer schedules
   * flip which side wins, which is how this was found.) */
  {
    int rc2 = lt_pthread_detach(th);
    CHECK(rc2 == EINVAL || rc2 == ESRCH);
    rc2 = lt_pthread_join(th, NULL);
    CHECK(rc2 == EINVAL || rc2 == ESRCH);
  }
  /* Unknown handle. */
  {
    pthread_t bogus;
    bogus.control = NULL;
    bogus.tid = 0;
    CHECK(lt_pthread_detach(bogus) == ESRCH);
    CHECK(lt_pthread_join(bogus, NULL) == ESRCH);
  }
  return fails;
}

/* ---- case: detached create / reap stress (review H1 + H2) -------------
 * Several threads concurrently create detached threads, create
 * joinable ones and detach them, while every create/detach runs
 * the reaper. Before the fixes this raced (H2) a detached record's
 * retire+free against its own pthread_create still writing
 * rec->handle / *thread, and (H1) the weave sweep freeing a control
 * block a queued record later read. Run it under ASan/TSan for the
 * strongest signal; the plain build checks completion and balance. */
static volatile uint32_t g_stress_runs;

static void* entry_stress_leaf(void* arg) {
  (void)arg;
  __atomic_add_fetch(&g_stress_runs, 1u, __ATOMIC_SEQ_CST);
  return NULL;
}

static volatile uint32_t g_stress_errs;

static void* entry_stress_driver(void* arg) {
  int i;
  int mode = (int)(uintptr_t)arg;
  pthread_attr_t attr;
  lt_pthread_attr_init(&attr);
  lt_pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (i = 0; i < 200; ++i) {
    pthread_t th;
    if (mode == 0 || (i & 1) == 0) {
      if (lt_pthread_create(&th, &attr, entry_stress_leaf, NULL) != 0) {
        __atomic_add_fetch(&g_stress_errs, 1u, __ATOMIC_SEQ_CST);
      }
    } else {
      if (lt_pthread_create(&th, NULL, entry_stress_leaf, NULL) != 0 ||
          lt_pthread_detach(th) != 0) {
        __atomic_add_fetch(&g_stress_errs, 1u, __ATOMIC_SEQ_CST);
      }
    }
  }
  lt_pthread_attr_destroy(&attr);
  return NULL;
}

static int case_detach_reap_stress(void) {
  int fails = 0;
  enum { D = 4 };
  pthread_t drv[D];
  int i;
  uint64_t deadline;
  __atomic_store_n(&g_stress_runs, 0u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_stress_errs, 0u, __ATOMIC_SEQ_CST);
  for (i = 0; i < D; ++i) {
    CHECK(lt_pthread_create(&drv[i], NULL, entry_stress_driver,
                            (void*)(uintptr_t)(i & 1)) == 0);
  }
  for (i = 0; i < D; ++i) CHECK(lt_pthread_join(drv[i], NULL) == 0);
  CHECK(g_stress_errs == 0u);
  deadline = now_ms() + 30000ull;
  while (__atomic_load_n(&g_stress_runs, __ATOMIC_SEQ_CST) < D * 200u &&
         now_ms() < deadline) {
    nap_ms(1);
  }
  CHECK(g_stress_runs == D * 200u);
  CHECK(settle_balance(500));
  CHECK_BALANCED();
  return fails;
}

/* ---- case: mutex contention exactness ---------------------------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_counter; /* guarded by g_mu only */

static void* entry_hammer(void* arg) {
  int i;
  (void)arg;
  for (i = 0; i < 50000; ++i) {
    lt_pthread_mutex_lock(&g_mu);
    ++g_counter;
    lt_pthread_mutex_unlock(&g_mu);
  }
  return NULL;
}

static int case_mutex_contention(void) {
  int fails = 0;
  enum { N = 8 };
  pthread_t th[N];
  int i;

  g_counter = 0;
  for (i = 0; i < N; ++i) {
    CHECK(lt_pthread_create(&th[i], NULL, entry_hammer, NULL) == 0);
  }
  for (i = 0; i < N; ++i) CHECK(lt_pthread_join(th[i], NULL) == 0);
  CHECK(g_counter == (unsigned long long)N * 50000ull);
  return fails;
}

/* ---- case: mutex flavors ------------------------------------------------- */
static pthread_mutex_t g_rmu; /* recursive, via attr */
static pthread_mutex_t g_emu; /* errorcheck, via attr */
static int g_helper_rc;

static void* entry_trylock_rmu(void* arg) {
  (void)arg;
  g_helper_rc = lt_pthread_mutex_trylock(&g_rmu);
  if (g_helper_rc == 0) lt_pthread_mutex_unlock(&g_rmu);
  return NULL;
}

static void* entry_unlock_emu(void* arg) {
  (void)arg;
  g_helper_rc = lt_pthread_mutex_unlock(&g_emu);
  return NULL;
}

static void* entry_timedlock_emu(void* arg) {
  struct timespec ts;
  (void)arg;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 150L * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  g_helper_rc = lt_pthread_mutex_timedlock(&g_emu, &ts);
  if (g_helper_rc == 0) lt_pthread_mutex_unlock(&g_emu);
  return NULL;
}

static int case_mutex_flavors(void) {
  int fails = 0;
  pthread_mutexattr_t attr;
  pthread_t th;
  uint64_t t0, t1;

  CHECK(lt_pthread_mutexattr_init(&attr) == 0);
  CHECK(lt_pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
  CHECK(lt_pthread_mutex_init(&g_rmu, &attr) == 0);
  CHECK(lt_pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
  CHECK(lt_pthread_mutex_init(&g_emu, &attr) == 0);
  CHECK(lt_pthread_mutexattr_destroy(&attr) == 0);

  /* Recursive: three levels deep, another thread sees EBUSY. */
  CHECK(lt_pthread_mutex_lock(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_lock(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_lock(&g_rmu) == 0);
  g_helper_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_trylock_rmu, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_helper_rc == EBUSY);
  CHECK(lt_pthread_mutex_unlock(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_unlock(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_unlock(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_unlock(&g_rmu) == EPERM); /* one too many */

  /* Errorcheck: self-relock is EDEADLK; foreign unlock is EPERM. */
  CHECK(lt_pthread_mutex_lock(&g_emu) == 0);
  CHECK(lt_pthread_mutex_lock(&g_emu) == EDEADLK);
  g_helper_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_unlock_emu, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_helper_rc == EPERM);

  /* Timedlock against the held mutex expires with ETIMEDOUT. */
  g_helper_rc = -1;
  t0 = now_ms();
  CHECK(lt_pthread_create(&th, NULL, entry_timedlock_emu, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  t1 = now_ms();
  CHECK(g_helper_rc == ETIMEDOUT);
  CHECK(t1 - t0 >= 100u && t1 - t0 < 30000u);
  CHECK(lt_pthread_mutex_unlock(&g_emu) == 0);
  /* Timedlock on a free mutex succeeds. */
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    CHECK(lt_pthread_mutex_timedlock(&g_emu, &ts) == 0);
    CHECK(lt_pthread_mutex_unlock(&g_emu) == 0);
  }

  /* Unlocking an unlocked normal mutex is EINVAL here. */
  {
    pthread_mutex_t nmu = PTHREAD_MUTEX_INITIALIZER;
    CHECK(lt_pthread_mutex_unlock(&nmu) == EINVAL);
    CHECK(lt_pthread_mutex_destroy(&nmu) == 0);
    CHECK(lt_pthread_mutex_lock(&nmu) == EINVAL); /* destroyed */
  }
  CHECK(lt_pthread_mutex_destroy(&g_rmu) == 0);
  CHECK(lt_pthread_mutex_destroy(&g_emu) == 0);
  return fails;
}

/* ---- case: condvar bounded queue, no missed wakeups ----------------------- */
#define QCAP 8
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_notempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_q_notfull = PTHREAD_COND_INITIALIZER;
static int g_queue[QCAP];
static int g_q_head, g_q_tail, g_q_count;
static long g_consumed_sum;
static int g_consumed_count;
static int g_order_ok;

static void* entry_producer(void* arg) {
  int i;
  (void)arg;
  for (i = 0; i < 10000; ++i) {
    lt_pthread_mutex_lock(&g_qmu);
    while (g_q_count == QCAP) lt_pthread_cond_wait(&g_q_notfull, &g_qmu);
    g_queue[g_q_tail] = i;
    g_q_tail = (g_q_tail + 1) % QCAP;
    ++g_q_count;
    lt_pthread_cond_signal(&g_q_notempty);
    lt_pthread_mutex_unlock(&g_qmu);
  }
  return NULL;
}

static void* entry_consumer(void* arg) {
  int expected = 0;
  (void)arg;
  while (expected < 10000) {
    int v;
    lt_pthread_mutex_lock(&g_qmu);
    while (g_q_count == 0) lt_pthread_cond_wait(&g_q_notempty, &g_qmu);
    v = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % QCAP;
    --g_q_count;
    lt_pthread_cond_signal(&g_q_notfull);
    lt_pthread_mutex_unlock(&g_qmu);
    if (v != expected) g_order_ok = 0;
    g_consumed_sum += v;
    ++g_consumed_count;
    ++expected;
  }
  return NULL;
}

static int case_cond_queue(void) {
  int fails = 0;
  pthread_t prod, cons;

  g_q_head = g_q_tail = g_q_count = 0;
  g_consumed_sum = 0;
  g_consumed_count = 0;
  g_order_ok = 1;
  CHECK(lt_pthread_create(&cons, NULL, entry_consumer, NULL) == 0);
  CHECK(lt_pthread_create(&prod, NULL, entry_producer, NULL) == 0);
  CHECK(lt_pthread_join(prod, NULL) == 0);
  CHECK(lt_pthread_join(cons, NULL) == 0);
  CHECK(g_consumed_count == 10000);
  CHECK(g_consumed_sum == (9999L * 10000L) / 2L);
  CHECK(g_order_ok == 1);
  return fails;
}

/* ---- case: condvar broadcast releases every waiter ------------------------- */
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_bcond = PTHREAD_COND_INITIALIZER;
static int g_bflag;
static volatile uint32_t g_bwoke;

static void* entry_bwaiter(void* arg) {
  (void)arg;
  lt_pthread_mutex_lock(&g_bmu);
  while (!g_bflag) lt_pthread_cond_wait(&g_bcond, &g_bmu);
  lt_pthread_mutex_unlock(&g_bmu);
  __atomic_add_fetch(&g_bwoke, 1u, __ATOMIC_SEQ_CST);
  return NULL;
}

static int case_cond_broadcast(void) {
  int fails = 0;
  enum { N = 4 };
  pthread_t th[N];
  int i;

  g_bflag = 0;
  __atomic_store_n(&g_bwoke, 0u, __ATOMIC_SEQ_CST);
  for (i = 0; i < N; ++i) {
    CHECK(lt_pthread_create(&th[i], NULL, entry_bwaiter, NULL) == 0);
  }
  nap_ms(200); /* let them all block */
  lt_pthread_mutex_lock(&g_bmu);
  g_bflag = 1;
  lt_pthread_cond_broadcast(&g_bcond);
  lt_pthread_mutex_unlock(&g_bmu);
  for (i = 0; i < N; ++i) CHECK(lt_pthread_join(th[i], NULL) == 0);
  CHECK(__atomic_load_n(&g_bwoke, __ATOMIC_SEQ_CST) == (uint32_t)N);
  return fails;
}

/* ---- case: condvar timed wait ------------------------------------------------ */
static int case_cond_timedwait(void) {
  int fails = 0;
  pthread_condattr_t attr;
  pthread_cond_t cond;
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  struct timespec ts;
  uint64_t t0, t1;
  int rc;

  CHECK(lt_pthread_condattr_init(&attr) == 0);
  CHECK(lt_pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0);
  CHECK(lt_pthread_cond_init(&cond, &attr) == 0);
  CHECK(lt_pthread_condattr_destroy(&attr) == 0);

  CHECK(lt_pthread_mutex_lock(&mu) == 0);
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_nsec += 150L * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  t0 = now_ms();
  rc = lt_pthread_cond_timedwait(&cond, &mu, &ts);
  t1 = now_ms();
  CHECK(rc == ETIMEDOUT);
  CHECK(t1 - t0 >= 100u && t1 - t0 < 30000u);
  /* The mutex is ours again on the timeout return. */
  CHECK(lt_pthread_mutex_unlock(&mu) == 0);

  /* A deadline already in the past expires at once. */
  CHECK(lt_pthread_mutex_lock(&mu) == 0);
  ts.tv_sec -= 10;
  rc = lt_pthread_cond_timedwait(&cond, &mu, &ts);
  CHECK(rc == ETIMEDOUT);
  CHECK(lt_pthread_mutex_unlock(&mu) == 0);

  /* Malformed abstime is EINVAL, before any waiting. */
  CHECK(lt_pthread_mutex_lock(&mu) == 0);
  ts.tv_nsec = 2000000000L;
  CHECK(lt_pthread_cond_timedwait(&cond, &mu, &ts) == EINVAL);
  CHECK(lt_pthread_mutex_unlock(&mu) == 0);

  CHECK(lt_pthread_cond_destroy(&cond) == 0);
  CHECK(lt_pthread_mutex_destroy(&mu) == 0);
  return fails;
}

/* ---- case: rwlock --------------------------------------------------------------- */
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile uint32_t g_writer_in;

static void* entry_writer_block(void* arg) {
  (void)arg;
  lt_pthread_rwlock_wrlock(&g_rw);
  __atomic_store_n(&g_writer_in, 1u, __ATOMIC_SEQ_CST);
  lt_pthread_rwlock_unlock(&g_rw);
  return NULL;
}

static int g_rw_rc;
static void* entry_tryrd(void* arg) {
  (void)arg;
  g_rw_rc = lt_pthread_rwlock_tryrdlock(&g_rw);
  if (g_rw_rc == 0) lt_pthread_rwlock_unlock(&g_rw);
  return NULL;
}
static void* entry_trywr(void* arg) {
  (void)arg;
  g_rw_rc = lt_pthread_rwlock_trywrlock(&g_rw);
  if (g_rw_rc == 0) lt_pthread_rwlock_unlock(&g_rw);
  return NULL;
}

static int case_rwlock(void) {
  int fails = 0;
  pthread_t th;

  /* Writer excludes both classes. */
  CHECK(lt_pthread_rwlock_wrlock(&g_rw) == 0);
  g_rw_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_tryrd, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_rw_rc == EBUSY);
  g_rw_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_trywr, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_rw_rc == EBUSY);
  CHECK(lt_pthread_rwlock_unlock(&g_rw) == 0);

  /* Readers hold concurrently; a writer cannot barge in. */
  CHECK(lt_pthread_rwlock_rdlock(&g_rw) == 0);
  g_rw_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_tryrd, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_rw_rc == 0);
  g_rw_rc = -1;
  CHECK(lt_pthread_create(&th, NULL, entry_trywr, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_rw_rc == EBUSY);

  /* A blocking writer waits for the last reader to leave. */
  __atomic_store_n(&g_writer_in, 0u, __ATOMIC_SEQ_CST);
  CHECK(lt_pthread_create(&th, NULL, entry_writer_block, NULL) == 0);
  nap_ms(150);
  CHECK(__atomic_load_n(&g_writer_in, __ATOMIC_SEQ_CST) == 0u);
  CHECK(lt_pthread_rwlock_unlock(&g_rw) == 0); /* main's read lock */
  CHECK(wait_flag(&g_writer_in, 30));
  CHECK(lt_pthread_join(th, NULL) == 0);

  CHECK(lt_pthread_rwlock_unlock(&g_rw) == EINVAL); /* not held */
  CHECK(lt_pthread_rwlock_destroy(&g_rw) == 0);
  return fails;
}

/* ---- case: once ---------------------------------------------------------------------- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static uint32_t g_once_runs;
static int g_once_value;

static void once_init(void) {
  __atomic_add_fetch(&g_once_runs, 1u, __ATOMIC_SEQ_CST);
  g_once_value = 42;
}

static void* entry_once(void* arg) {
  int i;
  (void)arg;
  for (i = 0; i < 100; ++i) lt_pthread_once(&g_once, once_init);
  return NULL;
}

static int case_once(void) {
  int fails = 0;
  enum { N = 8 };
  pthread_t th[N];
  int i;

  __atomic_store_n(&g_once_runs, 0u, __ATOMIC_SEQ_CST);
  for (i = 0; i < N; ++i) {
    CHECK(lt_pthread_create(&th[i], NULL, entry_once, NULL) == 0);
  }
  for (i = 0; i < N; ++i) CHECK(lt_pthread_join(th[i], NULL) == 0);
  CHECK(__atomic_load_n(&g_once_runs, __ATOMIC_SEQ_CST) == 1u);
  CHECK(g_once_value == 42);
  return fails;
}

/* ---- case: TLS keys --------------------------------------------------------------------- */
static pthread_key_t g_key1, g_key2;
static int g_dtor_log_key[32];
static int g_dtor_log_val[32];
static uint32_t g_dtor_n;

static void key_log(int key, int val) {
  uint32_t i = __atomic_fetch_add(&g_dtor_n, 1u, __ATOMIC_SEQ_CST);
  if (i < 32u) {
    g_dtor_log_key[i] = key;
    g_dtor_log_val[i] = val;
  }
}

static void dtor_key1(void* v) { key_log(1, (int)(uintptr_t)v); }

static void dtor_key2(void* v) {
  key_log(2, (int)(uintptr_t)v);
  /* Setting a value during the exit passes: the runner's next
   * pass must destroy the sentinel under key1 as well. */
  lt_pthread_setspecific(g_key1,
                         (void*)(uintptr_t)((int)(uintptr_t)v + 97));
}

static int g_get_ok;
static void* entry_keys(void* arg) {
  int base = (int)(uintptr_t)arg;
  lt_pthread_setspecific(g_key1, (void*)(uintptr_t)(base + 1));
  lt_pthread_setspecific(g_key2, (void*)(uintptr_t)(base + 2));
  if (lt_pthread_getspecific(g_key1) != (void*)(uintptr_t)(base + 1)) {
    g_get_ok = 0;
  }
  if (lt_pthread_getspecific(g_key2) != (void*)(uintptr_t)(base + 2)) {
    g_get_ok = 0;
  }
  return NULL;
}

static int case_keys(void) {
  int fails = 0;
  pthread_t a, b;
  uint32_t i, n;
  int seen[300];
  pthread_key_t key3;

  __atomic_store_n(&g_dtor_n, 0u, __ATOMIC_SEQ_CST);
  g_get_ok = 1;
  CHECK(lt_pthread_key_create(&g_key1, dtor_key1) == 0);
  CHECK(lt_pthread_key_create(&g_key2, dtor_key2) == 0);
  CHECK(lt_pthread_create(&a, NULL, entry_keys, (void*)(uintptr_t)100) == 0);
  CHECK(lt_pthread_create(&b, NULL, entry_keys, (void*)(uintptr_t)200) == 0);
  CHECK(lt_pthread_join(a, NULL) == 0);
  CHECK(lt_pthread_join(b, NULL) == 0);
  CHECK(g_get_ok == 1);
  /* Each thread produces exactly (1, base+1), (2, base+2),
   * (1, base+99): pass 1 destroys key1 then key2 (key2's
   * destructor re-arms key1 with base+2+97), pass 2 destroys
   * the sentinel. Verify the per-thread subsequences. */
  n = __atomic_load_n(&g_dtor_n, __ATOMIC_SEQ_CST);
  CHECK(n == 6u);
  memset(seen, 0, sizeof seen);
  {
    int expect_a[3] = {101, 102, 199};
    int expect_b[3] = {201, 202, 299};
    int ia = 0, ib = 0;
    for (i = 0; i < n && i < 32u; ++i) {
      int v = g_dtor_log_val[i];
      if (v >= 100 && v < 200) {
        CHECK(ia < 3 && v == expect_a[ia]);
        ++ia;
      } else if (v >= 200 && v < 300) {
        CHECK(ib < 3 && v == expect_b[ib]);
        ++ib;
      } else {
        CHECK(0); /* a value from nowhere */
      }
      seen[v] = 1;
    }
    CHECK(ia == 3 && ib == 3);
  }
  /* Delete makes values inert and set/get answer accordingly. */
  CHECK(lt_pthread_key_create(&key3, NULL) == 0);
  CHECK(lt_pthread_setspecific(key3, (void*)(uintptr_t)7) == 0);
  CHECK(lt_pthread_getspecific(key3) == (void*)(uintptr_t)7);
  CHECK(lt_pthread_key_delete(key3) == 0);
  CHECK(lt_pthread_getspecific(key3) == NULL);
  CHECK(lt_pthread_setspecific(key3, (void*)(uintptr_t)7) == EINVAL);
  CHECK(lt_pthread_key_delete(key3) == EINVAL);
  CHECK(lt_pthread_key_delete(g_key1) == 0);
  CHECK(lt_pthread_key_delete(g_key2) == 0);
  return fails;
}

/* ---- case: errno is per-thread ------------------------------------------------------------- */
static int g_errno_seen[2];
static int g_errno_stable[2];

static void* entry_errno(void* arg) {
  int idx = (int)(uintptr_t)arg;
  int mine = 222 + idx * 111; /* 222, 333 */
  int ok = 1;
  int i;
  LT_ERRNO = mine;
  for (i = 0; i < 60; ++i) {
    if (LT_ERRNO != mine) ok = 0;
    nap_ms(1);
  }
  g_errno_seen[idx] = LT_ERRNO;
  g_errno_stable[idx] = ok;
  return NULL;
}

static int case_errno(void) {
  int fails = 0;
  pthread_t a, b;

  LT_ERRNO = 111;
  CHECK(lt_pthread_create(&a, NULL, entry_errno, (void*)(uintptr_t)0) == 0);
  CHECK(lt_pthread_create(&b, NULL, entry_errno, (void*)(uintptr_t)1) == 0);
  CHECK(lt_pthread_join(a, NULL) == 0);
  CHECK(lt_pthread_join(b, NULL) == 0);
  CHECK(g_errno_seen[0] == 222 && g_errno_stable[0] == 1);
  CHECK(g_errno_seen[1] == 333 && g_errno_stable[1] == 1);
  CHECK(LT_ERRNO == 111);
  return fails;
}

/* ---- case: heap glue under churn -------------------------------------------------------------- */
static char g_glue_arena[8u * 1024u * 1024u];
static wh_heap_t* g_glue_heap;
static int g_glue_ok;

static void* entry_glue(void* arg) {
  int tid = (int)(uintptr_t)arg;
  int i;
  for (i = 0; i < 5000; ++i) {
    size_t n = 1u + (size_t)((i * 37 + tid * 11) % 256);
    unsigned char* p = (unsigned char*)wh_malloc(g_glue_heap, n);
    if (p == NULL) {
      g_glue_ok = 0;
      return NULL;
    }
    memset(p, tid + 1, n);
    if (p[0] != (unsigned char)(tid + 1) ||
        p[n - 1] != (unsigned char)(tid + 1)) {
      g_glue_ok = 0;
    }
    p = (unsigned char*)wh_realloc(g_glue_heap, p, n + 64u);
    if (p == NULL) {
      g_glue_ok = 0;
      return NULL;
    }
    if (p[0] != (unsigned char)(tid + 1)) g_glue_ok = 0;
    wh_free(g_glue_heap, p);
  }
  return NULL;
}

static int case_heap_glue(void) {
  int fails = 0;
  enum { N = 8 };
  pthread_t th[N];
  wh_options_t opts = lithothread_heap_options();
  wh_stats_t before, after;
  int i;

  g_glue_heap = wh_heap_create(g_glue_arena, sizeof g_glue_arena, &opts);
  CHECK(g_glue_heap != NULL);
  if (g_glue_heap == NULL) return fails;
  wh_heap_stats(g_glue_heap, &before);
  g_glue_ok = 1;
  for (i = 0; i < N; ++i) {
    CHECK(lt_pthread_create(&th[i], NULL, entry_glue,
                            (void*)(uintptr_t)i) == 0);
  }
  for (i = 0; i < N; ++i) CHECK(lt_pthread_join(th[i], NULL) == 0);
  CHECK(g_glue_ok == 1);
  wh_heap_stats(g_glue_heap, &after);
  CHECK(after.live_blocks == before.live_blocks);
  CHECK(after.free_bytes == before.free_bytes);
  wh_heap_destroy(g_glue_heap);
  g_glue_heap = NULL;
  return fails;
}

/* ---- case: pthread_exit --------------------------------------------------------------------------- */
static pthread_key_t g_exit_key;
static volatile uint32_t g_exit_dtor_ran;
static volatile uint32_t g_exit_after;

static void dtor_exit_key(void* v) {
  (void)v;
  __atomic_store_n(&g_exit_dtor_ran, 1u, __ATOMIC_SEQ_CST);
}

static void* entry_exit(void* arg) {
  (void)arg;
  lt_pthread_setspecific(g_exit_key, (void*)(uintptr_t)9);
  lt_pthread_exit((void*)(uintptr_t)4242);
  __atomic_store_n(&g_exit_after, 1u, __ATOMIC_SEQ_CST); /* unreachable */
  return NULL;
}

static int case_pthread_exit(void) {
  int fails = 0;
  pthread_t th;
  void* rv = NULL;

  __atomic_store_n(&g_exit_dtor_ran, 0u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_exit_after, 0u, __ATOMIC_SEQ_CST);
  CHECK(lt_pthread_key_create(&g_exit_key, dtor_exit_key) == 0);
  CHECK(lt_pthread_create(&th, NULL, entry_exit, NULL) == 0);
  CHECK(lt_pthread_join(th, &rv) == 0);
  CHECK(rv == (void*)(uintptr_t)4242);
  CHECK(__atomic_load_n(&g_exit_after, __ATOMIC_SEQ_CST) == 0u);
  CHECK(__atomic_load_n(&g_exit_dtor_ran, __ATOMIC_SEQ_CST) == 1u);
  CHECK(lt_pthread_key_delete(g_exit_key) == 0);
  return fails;
}

/* ==================================================================
 * Audit fix pass 2 regressions (Audit-SliceC-2026-10-28). The
 * first two cases are modeled directly on the audit's probes and
 * convict the pre-fix lithothread.c; they were baselined against
 * a stashed copy of the original before the fixes landed.
 * Cross-thread data follows the file's discipline: __atomic
 * builtins on shared words, plain accesses only behind a join or
 * an atomic happens-before edge.
 * ================================================================== */

/* Balance discipline for these cases: case_keys leaves the main
 * thread's key_values array allocated for the rest of the
 * process (the main record is process-lifetime by design, and
 * case_keys itself therefore never ends with CHECK_BALANCED), so
 * the absolute baseline sits one allocation behind for every
 * case after it, on the pre-fix code exactly as on this code.
 * These cases assert balance relative to the net they entered
 * with: the property under test is that each audit choreography
 * itself neither leaks nor double-frees. */
#define CHECK_REL_BALANCED(net0) CHECK(alloc_net() == (net0))

/* --- C-F2: a detached record is freed only once its thread has
 * fully returned (tid cell reads 0). T's key destructor registers
 * a second, weave-level destructor; that destructor's pass runs
 * after the key runner retired T's record, and it still reaches
 * the record (errno, getspecific) while B's create-time reap
 * fires in between. Sentinels both directions: T must read its
 * own values, B must read its own values, untouched. --- */
static pthread_key_t g_f2_key;
static int32_t g_f2_t_tid;
static uint32_t g_f2_in_pass2;
static int g_f2_b_ready;
static int g_f2_t_done;
static int g_f2_t_errno;
static uintptr_t g_f2_t_value;
static int g_f2_b_errno;
static uintptr_t g_f2_b_value;
static int g_f2_inner_fails;

static void f2_second_dtor(void* arg) {
  (void)arg;
  __atomic_store_n(&g_f2_in_pass2, 1u, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&g_f2_b_ready, __ATOMIC_SEQ_CST) == 0) {
  }
  g_f2_t_errno = LT_ERRNO;
  g_f2_t_value = (uintptr_t)lt_pthread_getspecific(g_f2_key);
  LT_ERRNO = 0xBAD;
  __atomic_store_n(&g_f2_t_done, 1, __ATOMIC_SEQ_CST);
}

static void f2_key_dtor(void* v) {
  (void)v;
  if (lw_current_tid() ==
      __atomic_load_n(&g_f2_t_tid, __ATOMIC_SEQ_CST)) {
    (void)lw_thread_atexit(f2_second_dtor, NULL);
  }
}

static void* f2_t_entry(void* arg) {
  (void)arg;
  __atomic_store_n(&g_f2_t_tid, lw_current_tid(), __ATOMIC_SEQ_CST);
  LT_ERRNO = 555;
  if (lt_pthread_setspecific(g_f2_key, (void*)(uintptr_t)0x1111) != 0) {
    __atomic_store_n(&g_f2_inner_fails, 1, __ATOMIC_SEQ_CST);
  }
  return NULL;
}

static void* f2_b_entry(void* arg) {
  (void)arg;
  LT_ERRNO = 777;
  if (lt_pthread_setspecific(g_f2_key, (void*)(uintptr_t)0x2222) != 0) {
    __atomic_store_n(&g_f2_inner_fails, 1, __ATOMIC_SEQ_CST);
  }
  __atomic_store_n(&g_f2_b_ready, 1, __ATOMIC_SEQ_CST);
  while (__atomic_load_n(&g_f2_t_done, __ATOMIC_SEQ_CST) == 0) {
  }
  g_f2_b_errno = LT_ERRNO;
  g_f2_b_value = (uintptr_t)lt_pthread_getspecific(g_f2_key);
  return NULL;
}

static int case_audit_record_gate(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_t th;
  pthread_t t_handle;
  const uint32_t* cell;
  uint64_t deadline;

  __atomic_store_n(&g_f2_t_tid, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_f2_in_pass2, 0u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_f2_b_ready, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_f2_t_done, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_f2_inner_fails, 0, __ATOMIC_SEQ_CST);
  g_f2_t_errno = 0;
  g_f2_t_value = 0;
  g_f2_b_errno = 0;
  g_f2_b_value = 0;

  CHECK(lt_pthread_key_create(&g_f2_key, f2_key_dtor) == 0);
  CHECK(lt_pthread_create(&th, NULL, f2_t_entry, NULL) == 0);
  t_handle = th;
  CHECK(lt_pthread_detach(th) == 0);
  CHECK(wait_flag((volatile uint32_t*)&g_f2_in_pass2, 30));
  /* T is inside its second destructor pass: still running, so
   * its tid cell must still read its tid (and its control block
   * is therefore unreaped and safe to read). */
  cell = lw_thread_cell(t_handle);
  CHECK(cell != NULL);
  if (cell != NULL) {
    CHECK(__atomic_load_n(cell, __ATOMIC_SEQ_CST) != 0u);
  }
  /* B's create fires the reap that used to free T's record
   * mid-pass. B then holds the record's memory up in the open:
   * any cross-thread stomp lands on B's sentinels. */
  CHECK(lt_pthread_create(&th, NULL, f2_b_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  /* T has fully returned once the cell reads 0; stop there (the
   * block may be swept by any later weave call). */
  if (cell != NULL) {
    deadline = now_ms() + 30000ull;
    while (__atomic_load_n(cell, __ATOMIC_SEQ_CST) != 0u) {
      CHECK(now_ms() < deadline);
      if (fails > 0) break;
      nap_ms(1);
    }
  }
  CHECK(__atomic_load_n(&g_f2_inner_fails, __ATOMIC_SEQ_CST) == 0);
  CHECK(g_f2_t_errno == 555);
  CHECK(g_f2_t_value == 0); /* its own slot, already consumed */
  CHECK(g_f2_b_errno == 777);
  CHECK(g_f2_b_value == (uintptr_t)0x2222);
  /* Churn once more through the create head: the queued record
   * must now actually be freed (the gate delays, never leaks). */
  CHECK(lt_pthread_create(&th, NULL, entry_index, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(lt_pthread_key_delete(g_f2_key) == 0);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* --- C-F1: rwlock missed wakeup. The writer holds ~30 us a
 * pass; readers take 250 ms deadlines. On the pre-fix code the
 * snapshot-after-attempt order slept readers on a free lock for
 * whole deadlines (audit probe: spikes to 266 ms, one full
 * timeout in 25 s). This case is the contention coverage: a few
 * seconds under a toggling writer, convicting on a full timeout
 * or a deadline-scale latency. On a two-core host, scheduler
 * noise swamps the probe's finer spike signature, so the sharp
 * conviction instrument for this finding is the rendezvous case
 * that follows it. --- */
static pthread_rwlock_t g_f1_rw;
static int g_f1_stop;
static long g_f1_attempts;
static long g_f1_spikes;
static long g_f1_timeouts;
static int g_f1_inner_fails;

static void f1_hold_us(long us) {
  struct timespec a;
  struct timespec b;
  (void)clock_gettime(CLOCK_MONOTONIC, &a);
  for (;;) {
    long d;
    (void)clock_gettime(CLOCK_MONOTONIC, &b);
    d = (b.tv_sec - a.tv_sec) * 1000000L +
        (b.tv_nsec - a.tv_nsec) / 1000L;
    if (d >= us) break;
  }
}

static void* f1_writer_entry(void* arg) {
  (void)arg;
  while (__atomic_load_n(&g_f1_stop, __ATOMIC_SEQ_CST) == 0) {
    if (lt_pthread_rwlock_wrlock(&g_f1_rw) != 0) {
      __atomic_store_n(&g_f1_inner_fails, 1, __ATOMIC_SEQ_CST);
      return NULL;
    }
    f1_hold_us(30);
    if (lt_pthread_rwlock_unlock(&g_f1_rw) != 0) {
      __atomic_store_n(&g_f1_inner_fails, 1, __ATOMIC_SEQ_CST);
      return NULL;
    }
  }
  return NULL;
}

static uint64_t g_f1_max[4];

static void* f1_reader_entry(void* arg) {
  int slot = (int)(uintptr_t)arg;
  uint64_t my_max = 0;
  while (__atomic_load_n(&g_f1_stop, __ATOMIC_SEQ_CST) == 0) {
    struct timespec ts;
    uint64_t t0;
    uint64_t lat;
    int rc;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 250L * 1000L * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000L;
    }
    t0 = now_ms();
    rc = lt_pthread_rwlock_timedrdlock(&g_f1_rw, &ts);
    lat = now_ms() - t0;
    (void)__sync_fetch_and_add(&g_f1_attempts, 1L);
    if (rc == ETIMEDOUT) {
      (void)__sync_fetch_and_add(&g_f1_timeouts, 1L);
      continue;
    }
    if (rc != 0) {
      __atomic_store_n(&g_f1_inner_fails, 1, __ATOMIC_SEQ_CST);
      g_f1_max[slot] = my_max;
      return NULL;
    }
    if (lat > 10) (void)__sync_fetch_and_add(&g_f1_spikes, 1L);
    if (lat > my_max) my_max = lat;
    if (lt_pthread_rwlock_unlock(&g_f1_rw) != 0) {
      __atomic_store_n(&g_f1_inner_fails, 1, __ATOMIC_SEQ_CST);
      g_f1_max[slot] = my_max;
      return NULL;
    }
  }
  g_f1_max[slot] = my_max;
  return NULL;
}

static int case_audit_rwlock_wakeup(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_t w;
  pthread_t r[4];
  int i;
  uint64_t t_end;

  CHECK(lt_pthread_rwlock_init(&g_f1_rw, NULL) == 0);
  __atomic_store_n(&g_f1_stop, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_f1_inner_fails, 0, __ATOMIC_SEQ_CST);
  g_f1_attempts = 0;
  g_f1_spikes = 0;
  g_f1_timeouts = 0;
  CHECK(lt_pthread_create(&w, NULL, f1_writer_entry, NULL) == 0);
  for (i = 0; i < 4; i++) {
    g_f1_max[i] = 0;
    CHECK(lt_pthread_create(&r[i], NULL, f1_reader_entry,
                            (void*)(uintptr_t)i) == 0);
  }
  t_end = now_ms() + 4000ull;
  while (now_ms() < t_end &&
         __atomic_load_n(&g_f1_attempts, __ATOMIC_SEQ_CST) < 400000L) {
    nap_ms(50);
  }
  __atomic_store_n(&g_f1_stop, 1, __ATOMIC_SEQ_CST);
  CHECK(lt_pthread_join(w, NULL) == 0);
  for (i = 0; i < 4; i++) {
    CHECK(lt_pthread_join(r[i], NULL) == 0);
  }
  CHECK(lt_pthread_rwlock_destroy(&g_f1_rw) == 0);
  {
    uint64_t mx = 0;
    for (i = 0; i < 4; i++) {
      if (g_f1_max[i] > mx) mx = g_f1_max[i];
    }
    printf("  [info] rwlock wakeup: %ld attempts, %ld spikes (>10 ms), "
           "%ld timeouts, max latency %llu ms\n",
           g_f1_attempts, g_f1_spikes, g_f1_timeouts,
           (unsigned long long)mx);
    /* The bug's signature is a sleep at deadline scale (the
     * probe's worst was 266 ms against a 250 ms deadline), not
     * scheduler noise: convict on any full timeout, and (in
     * plain builds) on any single latency past 100 ms. Spikes
     * past 10 ms are reported above for the record; a busy host
     * can produce those without any missed wakeup. Under the
     * sanitizer builds the scheduling inflation is so large
     * (this case's readers have consumed a full 250 ms deadline
     * on the fixed code under ASan, with the rendezvous case
     * below cycling cleanly in the same run) that latency
     * magnitudes carry no signal there, so the 100 ms line is
     * plain-build only; the timeout line and the rendezvous
     * case carry every build. */
    CHECK(__atomic_load_n(&g_f1_inner_fails, __ATOMIC_SEQ_CST) == 0);
    CHECK(g_f1_attempts > 10000L); /* the loop really ran */
    CHECK(g_f1_timeouts == 0);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(mx <= 100);
#endif
  }
  settle_balance(50);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* --- C-F1, the conviction instrument: a one-reader rendezvous
 * that turns the missed wakeup into a permanent, observable
 * freeze. Writer and reader rendezvous per cycle: the writer
 * takes the write lock, publishes the cycle, waits for the
 * reader to arm (the reader arms, then attempts rdlock and
 * blocks), holds ~30 us, releases, and requires the reader's
 * done within 500 ms. On the pre-fix code the snapshot sat
 * after the failed CAS, so a release landing in that crack was
 * invisible to the sleeper on EITHER side: a missed reader
 * sleeps through the parked release (the writer's done watch
 * fires), and a missed writer sleeps on wseq forever after the
 * reader's only unlock bump has passed... the cycle counter
 * freezes outright, with the lock word reading 0, because the
 * reader is waiting for a cycle the writer will never publish.
 * Measured on this host against the stashed original: frozen at
 * cycle 85,338 (~6 s in), lock free, writer asleep on a stale
 * wseq. On the fixed code the same instrument ran 465,487
 * cycles in 30 s with no stall. The case passes if cycles keep
 * progressing for the whole window; a 2 s progress gap or the
 * done watch convicts. --- */
static pthread_rwlock_t g_trap_rw;
static int g_trap_stop;
static int g_trap_trapped;
static uint32_t g_trap_holding;
static uint32_t g_trap_arm;
static uint32_t g_trap_done;
static long g_trap_cycles;

int sched_yield(void);

static int trap_stopped(void) {
  return __atomic_load_n(&g_trap_stop, __ATOMIC_SEQ_CST) != 0;
}

static void* trap_writer_entry(void* arg) {
  uint32_t k = 0;
  (void)arg;
  while (!trap_stopped()) {
    uint64_t t0;
    k++;
    if (lt_pthread_rwlock_wrlock(&g_trap_rw) != 0) goto out;
    __atomic_store_n(&g_trap_holding, k, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_trap_arm, __ATOMIC_SEQ_CST) != k) {
      if (trap_stopped()) goto out;
      (void)sched_yield();
    }
    f1_hold_us(30);
    if (lt_pthread_rwlock_unlock(&g_trap_rw) != 0) goto out;
    (void)__sync_fetch_and_add(&g_trap_cycles, 1L);
    t0 = now_ms();
    while (__atomic_load_n(&g_trap_done, __ATOMIC_SEQ_CST) != k) {
      if (trap_stopped()) goto out;
      (void)sched_yield();
      if (now_ms() - t0 > 500ull) {
        __atomic_store_n(&g_trap_trapped, 1, __ATOMIC_SEQ_CST);
        goto out;
      }
    }
    __atomic_store_n(&g_trap_holding, 0u, __ATOMIC_SEQ_CST);
  }
out:
  __atomic_store_n(&g_trap_holding, 0u, __ATOMIC_SEQ_CST);
  /* Final bump: release a reader still asleep so it can exit. */
  if (lt_pthread_rwlock_wrlock(&g_trap_rw) == 0) {
    (void)lt_pthread_rwlock_unlock(&g_trap_rw);
  }
  return NULL;
}

static void* trap_reader_entry(void* arg) {
  uint32_t seen = 0;
  (void)arg;
  while (!trap_stopped()) {
    uint32_t k = __atomic_load_n(&g_trap_holding, __ATOMIC_SEQ_CST);
    if (k == 0u || k == seen) {
      (void)sched_yield();
      continue;
    }
    seen = k;
    __atomic_store_n(&g_trap_arm, k, __ATOMIC_SEQ_CST);
    if (lt_pthread_rwlock_rdlock(&g_trap_rw) != 0) return NULL;
    __atomic_store_n(&g_trap_done, k, __ATOMIC_SEQ_CST);
    (void)lt_pthread_rwlock_unlock(&g_trap_rw);
  }
  return NULL;
}

static int case_audit_rwlock_trap(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_t w;
  pthread_t r;
  uint64_t t0;
  uint64_t last_progress;
  long last_cyc = 0;
  int wedged = 0;

  CHECK(lt_pthread_rwlock_init(&g_trap_rw, NULL) == 0);
  __atomic_store_n(&g_trap_stop, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_trap_trapped, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_trap_holding, 0u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_trap_arm, 0u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_trap_done, 0u, __ATOMIC_SEQ_CST);
  g_trap_cycles = 0;
  CHECK(lt_pthread_create(&w, NULL, trap_writer_entry, NULL) == 0);
  CHECK(lt_pthread_create(&r, NULL, trap_reader_entry, NULL) == 0);
  t0 = now_ms();
  last_progress = t0;
  while (__atomic_load_n(&g_trap_trapped, __ATOMIC_SEQ_CST) == 0 &&
         now_ms() - t0 < 8000ull) {
    long c = __atomic_load_n(&g_trap_cycles, __ATOMIC_SEQ_CST);
    if (c != last_cyc) {
      last_cyc = c;
      last_progress = now_ms();
    } else if (now_ms() - last_progress > 2000ull) {
      wedged = 1;
      break;
    }
    nap_ms(50);
  }
  printf("  [info] rwlock trap: %ld cycles, trapped=%d wedged=%d "
         "(lock=%u rseq=%u wseq=%u)\n",
         __atomic_load_n(&g_trap_cycles, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_trap_trapped, __ATOMIC_SEQ_CST), wedged,
         __atomic_load_n(&g_trap_rw.lock, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_trap_rw.rseq, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_trap_rw.wseq, __ATOMIC_SEQ_CST));
  CHECK(__atomic_load_n(&g_trap_trapped, __ATOMIC_SEQ_CST) == 0);
  CHECK(wedged == 0);
  CHECK(__atomic_load_n(&g_trap_cycles, __ATOMIC_SEQ_CST) > 100L);
  __atomic_store_n(&g_trap_stop, 1, __ATOMIC_SEQ_CST);
  if (!wedged) {
    /* A trapped reader was already released by the writer's
     * final bump; a wedged writer is asleep on wseq for good
     * and cannot be joined (the pre-fix code's parting gift:
     * this path exists only there, and the suite moves on
     * without the join). */
    CHECK(lt_pthread_join(w, NULL) == 0);
    CHECK(lt_pthread_join(r, NULL) == 0);
    CHECK(lt_pthread_rwlock_destroy(&g_trap_rw) == 0);
    settle_balance(50);
    CHECK_REL_BALANCED(net0);
  }
  return fails;
}

/* --- C-F4: pthread_join answers the value the weave collected:
 * a thread that exits through lw_exit (bypassing the trampoline
 * and its record store) joins with its real value. --- */
static void* f4_return_entry(void* arg) {
  (void)arg;
  return (void*)(uintptr_t)0x5678;
}

static void* f4_weave_exit_entry(void* arg) {
  (void)arg;
  lw_exit((void*)(uintptr_t)0x1234);
  return (void*)(uintptr_t)0x9999; /* unreachable */
}

static int case_audit_join_values(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_t th;
  void* rv = NULL;

  CHECK(lt_pthread_create(&th, NULL, f4_return_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, &rv) == 0);
  CHECK(rv == (void*)(uintptr_t)0x5678);
  rv = NULL;
  CHECK(lt_pthread_create(&th, NULL, f4_weave_exit_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, &rv) == 0);
  CHECK(rv == (void*)(uintptr_t)0x1234);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* --- C-F5: recursive count saturates at UINT32_MAX instead of
 * wrapping to 0 (which the unlock path misreads as the final
 * release). White-box: the count field is part of the public
 * pthread_mutex_t layout, so drive it to the boundary directly
 * instead of locking 2^32 times (~11 minutes at -O0). --- */
static pthread_mutex_t g_sat_mu;
static int g_sat_try;

static void* f5_trylock_entry(void* arg) {
  (void)arg;
  g_sat_try = lt_pthread_mutex_trylock(&g_sat_mu);
  if (g_sat_try == 0) (void)lt_pthread_mutex_unlock(&g_sat_mu);
  return NULL;
}

static int case_audit_mutex_count_sat(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_mutexattr_t at;
  pthread_t th;

  CHECK(lt_pthread_mutexattr_init(&at) == 0);
  CHECK(lt_pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE) == 0);
  CHECK(lt_pthread_mutex_init(&g_sat_mu, &at) == 0);
  CHECK(lt_pthread_mutexattr_destroy(&at) == 0);
  CHECK(lt_pthread_mutex_lock(&g_sat_mu) == 0); /* count 1 */
  g_sat_mu.count = UINT32_MAX;                  /* boundary, white-box */
  CHECK(lt_pthread_mutex_lock(&g_sat_mu) == 0); /* saturating relock */
  CHECK(g_sat_mu.count == UINT32_MAX);          /* not wrapped to 0 */
  g_sat_try = -1;
  CHECK(lt_pthread_create(&th, NULL, f5_trylock_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_sat_try == EBUSY); /* still owned at saturation */
  CHECK(lt_pthread_mutex_unlock(&g_sat_mu) == 0); /* count MAX-1 */
  g_sat_try = -1;
  CHECK(lt_pthread_create(&th, NULL, f5_trylock_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, NULL) == 0);
  CHECK(g_sat_try == EBUSY); /* one unlock did not release it */
  g_sat_mu.count = 1;        /* rewind for a clean release */
  CHECK(lt_pthread_mutex_unlock(&g_sat_mu) == 0);
  CHECK(lt_pthread_mutex_destroy(&g_sat_mu) == 0);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* --- C-F6: an abstime whose nanosecond conversion would wrap
 * is EINVAL, promptly... not an instant ETIMEDOUT off a wrapped
 * deadline. 18446744074 * 1e9 wraps uint64 (the threshold is
 * (2^64-1)/1e9 = 18446744073). --- */
static int case_audit_abstime_bound(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_mutex_t mu;
  pthread_rwlock_t rw;
  struct timespec ts;
  uint64_t t0;
  int rc;

  ts.tv_sec = (time_t)18446744074LL;
  ts.tv_nsec = 0;

  CHECK(lt_pthread_mutex_init(&mu, NULL) == 0);
  t0 = now_ms();
  rc = lt_pthread_mutex_timedlock(&mu, &ts);
  CHECK(rc == EINVAL);
  CHECK(now_ms() - t0 < 5000ull);
  CHECK(lt_pthread_mutex_destroy(&mu) == 0);

  CHECK(lt_pthread_rwlock_init(&rw, NULL) == 0);
  t0 = now_ms();
  rc = lt_pthread_rwlock_timedrdlock(&rw, &ts);
  CHECK(rc == EINVAL);
  CHECK(now_ms() - t0 < 5000ull);
  CHECK(lt_pthread_rwlock_destroy(&rw) == 0);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* --- C-F3: pthread_exit from a key destructor. The trampoline
 * has already returned when key destructors run, so the call
 * must delegate to the weave exit instead of longjmping into
 * the dead frame, and join must observe the destructor's value
 * (0x77), not the entry's return (0x55). Runs last: on the
 * pre-fix code this case is the wild one. --- */
static pthread_key_t g_f3_key;
static int g_f3_dtor_ran;
static int g_f3_after_exit;
static int g_f3_inner_fails;

static void f3_key_dtor(void* v) {
  (void)v;
  g_f3_dtor_ran = 1;
  lt_pthread_exit((void*)(uintptr_t)0x77);
  g_f3_after_exit = 1; /* must never execute */
}

static void* f3_entry(void* arg) {
  (void)arg;
  if (lt_pthread_setspecific(g_f3_key, (void*)(uintptr_t)0x1) != 0) {
    __atomic_store_n(&g_f3_inner_fails, 1, __ATOMIC_SEQ_CST);
  }
  return (void*)(uintptr_t)0x55;
}

static int case_audit_exit_from_dtor(void) {
  int fails = 0;
  long net0 = alloc_net();
  pthread_t th;
  void* rv = NULL;

  g_f3_dtor_ran = 0;
  g_f3_after_exit = 0;
  __atomic_store_n(&g_f3_inner_fails, 0, __ATOMIC_SEQ_CST);
  CHECK(lt_pthread_key_create(&g_f3_key, f3_key_dtor) == 0);
  CHECK(lt_pthread_create(&th, NULL, f3_entry, NULL) == 0);
  CHECK(lt_pthread_join(th, &rv) == 0);
  CHECK(__atomic_load_n(&g_f3_inner_fails, __ATOMIC_SEQ_CST) == 0);
  CHECK(g_f3_dtor_ran == 1);
  CHECK(g_f3_after_exit == 0);
  CHECK(rv == (void*)(uintptr_t)0x77);
  CHECK(lt_pthread_key_delete(g_f3_key) == 0);
  CHECK_REL_BALANCED(net0);
  return fails;
}

/* ---- setup + main ---------------------------------------------------------------------------------------- */
static pthread_mutex_t g_setup_mu = PTHREAD_MUTEX_INITIALIZER;

#define RUN_CASE(fn)                                   \
  do {                                                 \
    int f = fn();                                      \
    printf("%s: %s\n", f == 0 ? "PASS" : "FAIL", #fn); \
    g_failures += f;                                   \
  } while (0)

int main(void) {
  const size_t arena_bytes = 32u * 1024u * 1024u;
  void* arena;
  wh_options_t opts;
  wh_heap_t* heap;
  pthread_t th;
  int i;

  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(300);

  arena = mmap(NULL, arena_bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (arena == MAP_FAILED) {
    printf("FAIL: MAP_32BIT arena unavailable\n");
    return 1;
  }
#ifdef LT_NO_HOOK_DEFAULT_HEAP
  /* TSan build only (see the header's TSan paragraph): the
   * default heap runs without lock hooks so the interposed free
   * that ld.so calls from its TLS teardown never enters an
   * instrumented mutex; the glue hooks are still exercised by
   * case_heap_glue on its own heap in normal thread context. */
  memset(&opts, 0, sizeof opts);
#else
  opts = lithothread_heap_options();
#endif
  heap = wh_heap_create(arena, arena_bytes, &opts);
  if (heap == NULL) {
    printf("FAIL: wh_heap_create\n");
    return 1;
  }
  wh_set_default_heap(heap);
  if (lw_main_thread_init() != LITHOSYS_OK) {
    printf("FAIL: lw_main_thread_init\n");
    return 1;
  }
  __litholibc_errno_arm_tls();

  /* Warm-up: saturate the host libc's per-thread caches and the
   * lazy paths (weave attach, key runner, errno attach) before
   * the balance baseline is taken. */
  for (i = 0; i < 4; ++i) {
    if (lt_pthread_create(&th, NULL, entry_index, (void*)(uintptr_t)i) != 0 ||
        lt_pthread_join(th, NULL) != 0) {
      printf("FAIL: warm-up create/join\n");
      return 1;
    }
  }
  __atomic_store_n(&g_detach_done, 0u, __ATOMIC_SEQ_CST);
  {
    pthread_attr_t attr;
    lt_pthread_attr_init(&attr);
    lt_pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (lt_pthread_create(&th, &attr, entry_setflag, NULL) != 0) {
      printf("FAIL: warm-up detach\n");
      return 1;
    }
    lt_pthread_attr_destroy(&attr);
    if (!wait_flag(&g_detach_done, 30)) {
      printf("FAIL: warm-up detach never completed\n");
      return 1;
    }
  }
  if (lt_pthread_create(&th, NULL, entry_keys, (void*)(uintptr_t)300) != 0) {
    /* keys not yet created: entry_keys writes to key slots 0/1
     * only if they exist; before case_keys runs they do not, and
     * setspecific answers EINVAL, which the entry ignores. The
     * point here is exercising the create/join path once more. */
    printf("FAIL: warm-up keys thread\n");
    return 1;
  }
  lt_pthread_join(th, NULL);
  if (lt_pthread_create(&th, NULL, entry_errno, (void*)(uintptr_t)0) != 0 ||
      lt_pthread_join(th, NULL) != 0) {
    printf("FAIL: warm-up errno thread\n");
    return 1;
  }
  lt_pthread_mutex_lock(&g_setup_mu);
  lt_pthread_mutex_unlock(&g_setup_mu);
  /* Force the main thread's own lazy attachments (its LithoThread
   * record and errno slot ride it) before the baseline: the first
   * self() / errno touch inside a case would otherwise allocate
   * them after it and spoil the balance checks. */
  (void)lt_pthread_self();
  LT_ERRNO = 0;

  printf("setup: default heap (glue hooks) over MAP_32BIT arena, "
         "main attached, errno TLS armed\n");
  g_baseline_net = alloc_net();

  RUN_CASE(case_create_join);
  RUN_CASE(case_detach);
  RUN_CASE(case_detach_reap_stress);
  RUN_CASE(case_mutex_contention);
  RUN_CASE(case_mutex_flavors);
  RUN_CASE(case_cond_queue);
  RUN_CASE(case_cond_broadcast);
  RUN_CASE(case_cond_timedwait);
  RUN_CASE(case_rwlock);
  RUN_CASE(case_once);
  RUN_CASE(case_keys);
  RUN_CASE(case_errno);
  RUN_CASE(case_heap_glue);
  RUN_CASE(case_pthread_exit);
  RUN_CASE(case_audit_record_gate);
  RUN_CASE(case_audit_rwlock_wakeup);
  RUN_CASE(case_audit_rwlock_trap);
  RUN_CASE(case_audit_join_values);
  RUN_CASE(case_audit_mutex_count_sat);
  RUN_CASE(case_audit_abstime_bound);
  RUN_CASE(case_audit_exit_from_dtor);

  printf("LITHOTHREAD TEST: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
