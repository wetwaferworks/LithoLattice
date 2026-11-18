/* lockfree_stress_test.c ... host stress for the lock-free registry
 * (lithoweave.c) and the lock-free shared heap (lithothread_heap.c).
 *
 * Built natively against host libc with the LithoWeave native host
 * double, and run under -fsanitize=thread and -fsanitize=address:
 *   gcc -std=c11 -O1 -g -fno-pie -no-pie -fsanitize=thread \
 *     -iquote include -iquote src src/lithoweave.c \
 *     src/lithoweave_native.c src/waferheap.c src/lithothread_heap.c \
 *     tests/lockfree_stress_test.c -lpthread
 * (-no-pie because the frozen spawn word is 32 bits and the
 * registry is a static array.)
 *
 * Cases: multi-thread alloc/free with pattern checks, cross-thread
 * free and realloc through a hand-off ring, arena reuse across
 * waves (remote lists drained by the next owner), registry churn
 * (concurrent spawn/join/detach from several threads), and the
 * negative cases: full registry -> EAGAIN, stale and double join ->
 * EINVAL, foreign free -> no-op. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "lithoheap.h"
#include "lithoweave.h"

#define REGION (512u << 20)
#define THREADS 8
#define ITERS 20000
#define RING 1024

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static uintptr_t g_region_end;
static int commit(uintptr_t end, void* user) { (void)user; return end <= g_region_end; }

static void* g_ring[RING]; /* hand-off slots, atomic exchange only */

static uint32_t xs(uint32_t* s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

typedef struct { int id; int cross; } heap_arg_t;

static void fill(void* p, size_t n, uint8_t tag) { memset(p, tag, n); }
static int verify(const void* p, size_t n, uint8_t tag) {
  const uint8_t* b = (const uint8_t*)p; size_t i;
  for (i = 0; i < n; ++i) if (b[i] != tag) return 0;
  return 1;
}

/* Each block carries [size_t len][uint8 tag] then tag bytes. */
static void* heap_worker(void* vp) {
  heap_arg_t* a = (heap_arg_t*)vp;
  uint32_t s = 0x9E3779B9u ^ (uint32_t)(a->id * 7919 + 1);
  void* mine[64] = {0};
  int i;
  for (i = 0; i < ITERS; ++i) {
    uint32_t r = xs(&s);
    int k = (int)(r % 64u);
    if (mine[k] != NULL) {
      size_t len = *(size_t*)mine[k];
      uint8_t tag = ((uint8_t*)mine[k])[sizeof(size_t)];
      if (!verify((uint8_t*)mine[k] + sizeof(size_t) + 1, len, tag)) { CHECK(0); }
      if (a->cross && (r & 1u)) {
        void* prev = __atomic_exchange_n(&g_ring[(r >> 8) % RING], mine[k], __ATOMIC_ACQ_REL);
        if (prev != NULL) {
          size_t pl = *(size_t*)prev;
          uint8_t pt = ((uint8_t*)prev)[sizeof(size_t)];
          if (!verify((uint8_t*)prev + sizeof(size_t) + 1, pl, pt)) { CHECK(0); }
          if (r & 2u) {
            /* foreign realloc: content must survive the move */
            void* q = lh_realloc(prev, sizeof(size_t) + 1 + pl + 64);
            CHECK(q != NULL);
            if (q) { CHECK(verify((uint8_t*)q + sizeof(size_t) + 1, pl, pt)); lh_free(q); }
          } else {
            lh_free(prev); /* cross-thread free */
          }
        }
      } else {
        lh_free(mine[k]);
      }
      mine[k] = NULL;
    } else {
      size_t len = (r >> 4) % ((r & 0x100u) ? 8192u : 256u);
      void* p = (r & 0x200u) ? lh_aligned_alloc(64, sizeof(size_t) + 1 + len)
                             : lh_malloc(sizeof(size_t) + 1 + len);
      CHECK(p != NULL);
      if (p == NULL) continue;
      if (r & 0x200u) CHECK(((uintptr_t)p & 63u) == 0u);
      CHECK(lh_usable_size(p) >= sizeof(size_t) + 1 + len);
      *(size_t*)p = len;
      ((uint8_t*)p)[sizeof(size_t)] = (uint8_t)(a->id + 1);
      fill((uint8_t*)p + sizeof(size_t) + 1, len, (uint8_t)(a->id + 1));
      mine[k] = p;
    }
  }
  for (i = 0; i < 64; ++i) lh_free(mine[i]);
  lh_thread_release();
  return NULL;
}

static void run_heap_wave(int cross, int* arenas_seen) {
  pthread_t t[THREADS];
  heap_arg_t a[THREADS];
  int i;
  for (i = 0; i < THREADS; ++i) {
    a[i].id = i; a[i].cross = cross;
    pthread_create(&t[i], NULL, heap_worker, &a[i]);
  }
  for (i = 0; i < THREADS; ++i) pthread_join(t[i], NULL);
  (void)arenas_seen;
}

/* ---- registry ---- */
static void* entry_ret(void* p) { return p; }
static volatile int g_hold;
static void* entry_hold(void* p) {
  while (__atomic_load_n(&g_hold, __ATOMIC_ACQUIRE)) { }
  return p;
}

static void* churn_worker(void* vp) {
  int id = (int)(intptr_t)vp, i;
  for (i = 0; i < 400; ++i) {
    lw_descriptor_t d; lw_thread_t th; void* rv = NULL;
    lithosys_err_t rc;
    memset(&d, 0, sizeof d);
    d.entry = entry_ret; d.arg = (void*)(intptr_t)(id * 100000 + i);
    rc = lw_spawn(&d, &th);
    if (rc == LITHOSYS_EAGAIN) continue; /* registry momentarily full */
    CHECK(rc == LITHOSYS_OK);
    if (rc != LITHOSYS_OK) continue;
    if (i % 3 == 0) {
      CHECK(lw_detach(th) == LITHOSYS_OK);
      CHECK(lw_join(th, NULL) == LITHOSYS_EINVAL); /* detached: no join */
    } else {
      CHECK(lw_join(th, &rv) == LITHOSYS_OK);
      CHECK(rv == d.arg);
      CHECK(lw_join(th, &rv) == LITHOSYS_EINVAL); /* stale handle */
    }
  }
  return NULL;
}

static void case_registry_churn(void) {
  pthread_t t[4];
  int i;
  for (i = 0; i < 4; ++i) pthread_create(&t[i], NULL, churn_worker, (void*)(intptr_t)i);
  for (i = 0; i < 4; ++i) pthread_join(t[i], NULL);
}

/* Negative: fill the registry with held threads; the next spawn must
 * say EAGAIN (not crash, not reuse a live slot); after release and
 * join every slot comes back. */
static void case_registry_full(void) {
  static lw_thread_t th[LW_MAX_THREADS];
  lw_descriptor_t d;
  int n = 0, i;
  lithosys_err_t rc;
  memset(&d, 0, sizeof d);
  d.entry = entry_hold;
  __atomic_store_n(&g_hold, 1, __ATOMIC_RELEASE);
  for (;;) {
    rc = lw_spawn(&d, &th[n]);
    if (rc != LITHOSYS_OK) break;
    if (++n == (int)LW_MAX_THREADS) { rc = lw_spawn(&d, &th[0]); break; }
  }
  CHECK(rc == LITHOSYS_EAGAIN);
  CHECK(n >= (int)LW_MAX_THREADS - 2); /* the main attach may hold one */
  __atomic_store_n(&g_hold, 0, __ATOMIC_RELEASE);
  for (i = 0; i < n; ++i) CHECK(lw_join(th[i], NULL) == LITHOSYS_OK);
  /* all free again: a full round must succeed */
  d.entry = entry_ret;
  for (i = 0; i < n; ++i) CHECK(lw_spawn(&d, &th[i]) == LITHOSYS_OK);
  for (i = 0; i < n; ++i) CHECK(lw_join(th[i], NULL) == LITHOSYS_OK);
  /* a handle that never came from the registry */
  {
    lw_thread_t bogus; int x;
    bogus.control = (lw_control_t*)(void*)&x; bogus.tid = 5;
    CHECK(lw_join(bogus, NULL) == LITHOSYS_EINVAL);
    CHECK(lw_detach(bogus) == LITHOSYS_EINVAL);
  }
}

int main(void) {
  void* region = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  int stack_local = 0, wave;
  CHECK(region != MAP_FAILED);
  g_region_end = (uintptr_t)region + REGION;
  CHECK(lh_malloc(16) == NULL); /* negative: not installed yet */
  CHECK(lh_install((uintptr_t)region, g_region_end, commit, NULL) == 1);

  for (wave = 0; wave < 3; ++wave) run_heap_wave(1, NULL);
  {
    int i;
    for (i = 0; i < RING; ++i) { void* p = g_ring[i]; g_ring[i] = NULL; lh_free(p); }
  }
  /* A wave with no hand-offs reclaims the released arenas and
   * drains every remote list left from the cross waves. */
  run_heap_wave(0, NULL);
  CHECK(lh_debug_remote_pending() == 0u);
  printf("PASS: heap alloc/free, cross-thread free/realloc, arena reuse\n");

  /* negative: foreign and NULL frees are no-ops */
  lh_free(&stack_local);
  lh_free(NULL);
  CHECK(lh_usable_size(&stack_local) == 0u);
  CHECK(lh_realloc(&stack_local, 32) == NULL);
  printf("PASS: foreign pointers refused\n");

  case_registry_churn();
  printf("PASS: registry churn\n");
  case_registry_full();
  printf("PASS: registry full / stale handles\n");
  printf(g_fail ? "LOCKFREE STRESS: FAIL\n" : "LOCKFREE STRESS: PASS\n");
  return g_fail;
}
