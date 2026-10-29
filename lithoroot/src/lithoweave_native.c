/* lithoweave_native.c ... the POSIX host back-end for LithoWeave.
 *
 * TEST SCAFFOLDING. This file plays the HOST's role so the weave
 * can be exercised natively: it implements lithosys_thread_spawn
 * over pthread_create, the host side of the frozen tid-cell exit
 * protocol, and the module-side cell wait the weave links against
 * off-wasm. On the web host every one of these jobs belongs to the
 * page layer (lkboot); a future native LithoSys back-end replaces
 * this file whole. It is compiled WITHOUT the LithoRoot include
 * directory (host headers only, LithoRoot headers reached by
 * relative path) so its calls to host libc are the real ones.
 *
 * The protocol, host side, exactly as frozen: the wrapper thread
 * calls the module's lithosys_thread_start(start_arg); when it
 * returns, the wrapper stores 0 into the tid cell (the first word
 * at start_arg) with a seq-cst store and then notifies the cell's
 * address. Joiners in the module wait on the cell.
 *
 * Two wasm shapes show through and are handled deliberately:
 *
 *   - start_arg is a uint32_t because on wasm it is a linear
 *     memory address. Here it must carry a real host pointer. The
 *     test harness guarantees the pointers fit: control blocks
 *     come from LithoLibC malloc over a WaferHeap arena the test
 *     maps below 4 GiB (MAP_32BIT on Linux), so the truncation in
 *     lw_spawn's cast and the widening here are both lossless.
 *     This is a harness accommodation, not a design property: the
 *     frozen spawn ABI is a wasm ABI.
 *
 *   - Portable native C has no futex, so lw__cell_wait is a
 *     mutex / condvar registry keyed by the cell address. The
 *     module-visible contract (block while the cell holds the
 *     expected value; wake on notify; tolerate spurious wakeups)
 *     is identical to the wasm wait32 the weave uses on target.
 *
 * lithosys_thread_spawn reads the tid out of the cell and the
 * stack request out of the second spawn word (lithoweave.h), and
 * returns the tid on success, or the negation of a lithosys_err_t
 * code. EAGAIN is the only refusal for thread exhaustion, per the
 * freeze's spirit: no pool cap here beyond a generous record
 * table; a refused spawn means pthread_create itself failed or
 * the table is full.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "../include/lithosys.h"
#include "../include/lithoweave.h"
#include "lithoweave_internal.h"

/* ---- cell wait / notify: the condvar registry ---------------------- */

#define LW__NATIVE_CELL_SLOTS 64

typedef struct {
  const volatile uint32_t* cell;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  unsigned waiters;
  int used;
} lw__cell_slot_t;

static lw__cell_slot_t lw__slots[LW__NATIVE_CELL_SLOTS];
static pthread_mutex_t lw__slots_mu = PTHREAD_MUTEX_INITIALIZER;

/* Find the slot for a cell, creating it when create is set.
 * Called with no slot lock held; returns the slot with its
 * mutex LOCKED, or NULL (creating nothing) when the table is
 * full or the cell has no slot and create is clear. */
static lw__cell_slot_t* lw__slot_acquire(const volatile uint32_t* cell,
                                         int create) {
  int i;
  lw__cell_slot_t* free_slot = NULL;
  pthread_mutex_lock(&lw__slots_mu);
  for (i = 0; i < LW__NATIVE_CELL_SLOTS; ++i) {
    lw__cell_slot_t* s = &lw__slots[i];
    if (s->used && s->cell == cell) {
      pthread_mutex_lock(&s->mu);
      pthread_mutex_unlock(&lw__slots_mu);
      return s;
    }
    if (!s->used && free_slot == NULL) free_slot = s;
  }
  if (create && free_slot != NULL) {
    free_slot->used = 1;
    free_slot->cell = cell;
    free_slot->waiters = 0;
    pthread_mutex_init(&free_slot->mu, NULL);
    pthread_cond_init(&free_slot->cv, NULL);
    pthread_mutex_lock(&free_slot->mu);
    pthread_mutex_unlock(&lw__slots_mu);
    return free_slot;
  }
  pthread_mutex_unlock(&lw__slots_mu);
  return NULL;
}

void lw__cell_wait(const volatile uint32_t* cell, uint32_t expected) {
  lw__cell_slot_t* s = lw__slot_acquire(cell, 1);
  if (s == NULL) {
    /* Table full: fall back to a short sleep and let the caller's
     * loop re-check. Test-scale code never reaches this; it exists
     * so a full table degrades instead of deadlocking. */
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L; /* 1 ms */
    nanosleep(&ts, NULL);
    return;
  }
  s->waiters++;
  while (__atomic_load_n((const uint32_t*)cell, __ATOMIC_SEQ_CST) == expected) {
    pthread_cond_wait(&s->cv, &s->mu);
  }
  s->waiters--;
  pthread_mutex_unlock(&s->mu);
}

void lw__cell_notify_all(const volatile uint32_t* cell) {
  lw__cell_slot_t* s = lw__slot_acquire(cell, 0);
  if (s == NULL) return; /* nobody is waiting on this cell */
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
}

/* Missed-wakeup audit for the wait / notify pair: the waiter
 * checks the cell value under the slot lock before waiting, and
 * the notifier's store happens-before its slot lookup, so a
 * notify that finds no slot is always followed by the waiter
 * observing the stored 0 and never blocking. The ordering is:
 * wrapper stores 0, then notify_all looks the slot up; waiter
 * creates / finds the slot, then checks the value under the slot
 * lock. If the store preceded the check, the check sees 0. If it
 * follows, the broadcast reaches the held slot. No third case. */

/* ---- spawn records --------------------------------------------------- */

#define LW__NATIVE_MAX_THREADS 256

typedef struct {
  uint32_t start_arg;
  int used;
} lw__native_rec_t;

static lw__native_rec_t lw__recs[LW__NATIVE_MAX_THREADS];
static pthread_mutex_t lw__recs_mu = PTHREAD_MUTEX_INITIALIZER;

/* Test hook: force the next n spawn calls to refuse with EAGAIN,
 * so the weave's spawn failure path can be exercised. */
static volatile int lw__fail_next = 0;

void lw__native_test_fail_spawn(int count) { lw__fail_next = count; }

static void* lw__native_thread_main(void* p) {
  lw__native_rec_t* rec = (lw__native_rec_t*)p;
  uint32_t start_arg = rec->start_arg;
  volatile uint32_t* cell = (volatile uint32_t*)(uintptr_t)start_arg;

  lithosys_thread_start(start_arg);

  /* The frozen protocol, host side: seq-cst store of 0 into the
   * tid cell, then notify its address. The store must precede the
   * record release and the notify: a joiner that observes the 0
   * may reap the control block as soon as it wakes. */
  __atomic_store_n((uint32_t*)cell, 0u, __ATOMIC_SEQ_CST);
  lw__cell_notify_all(cell);

  pthread_mutex_lock(&lw__recs_mu);
  rec->used = 0;
  pthread_mutex_unlock(&lw__recs_mu);
  return NULL;
}

int32_t lithosys_thread_spawn(uint32_t start_arg) {
  volatile uint32_t* base = (volatile uint32_t*)(uintptr_t)start_arg;
  int32_t tid = (int32_t)__atomic_load_n((const uint32_t*)base,
                                        __ATOMIC_SEQ_CST);
  uint32_t stack_bytes = __atomic_load_n(
      (const uint32_t*)((const char*)base + LW_SPAWN_STACK_OFFSET),
      __ATOMIC_SEQ_CST);
  lw__native_rec_t* rec = NULL;
  pthread_attr_t attr;
  pthread_t pt;
  int i, rc;

  if (lw__fail_next > 0) {
    lw__fail_next--;
    return -LITHOSYS_EAGAIN;
  }

  pthread_mutex_lock(&lw__recs_mu);
  for (i = 0; i < LW__NATIVE_MAX_THREADS; ++i) {
    if (!lw__recs[i].used) {
      rec = &lw__recs[i];
      rec->used = 1;
      rec->start_arg = start_arg;
      break;
    }
  }
  pthread_mutex_unlock(&lw__recs_mu);
  if (rec == NULL) return -LITHOSYS_EAGAIN;

  pthread_attr_init(&attr);
  if (stack_bytes >= 16384u) {
    if (pthread_attr_setstacksize(&attr, (size_t)stack_bytes) != 0) {
      pthread_attr_destroy(&attr);
      pthread_mutex_lock(&lw__recs_mu);
      rec->used = 0;
      pthread_mutex_unlock(&lw__recs_mu);
      return -LITHOSYS_EINVAL;
    }
  }
  rc = pthread_create(&pt, &attr, lw__native_thread_main, rec);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    pthread_mutex_lock(&lw__recs_mu);
    rec->used = 0;
    pthread_mutex_unlock(&lw__recs_mu);
    return -LITHOSYS_EAGAIN;
  }
  /* The module joins through the cell, never through pthread_join,
   * so the host thread is detached: its record release in the
   * wrapper is the last host-side act. */
  pthread_detach(pt);
  return tid;
}

/* ---- process control stubs -------------------------------------------
 * lithoweave.c references lithosys_exit for its main-thread
 * lw_exit path. The real back-ends own these; here they are the
 * host's own, so the weave links and runs standalone in tests. */

void lithosys_exit(int32_t code) { exit((int)code); }

void lithosys_abort(void) { abort(); }
