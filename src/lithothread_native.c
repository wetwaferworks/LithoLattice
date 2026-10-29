/* lithothread_native.c ... the POSIX host back-end for LithoThread's seam.
 *
 * TEST SCAFFOLDING, the sibling of lithoweave_native.c: it plays
 * the host for the two things LithoThread needs that are not the
 * weave's business:
 *
 *   - the wait / notify seam (lithothread_wait.h). On wasm these
 *     are instructions on shared memory; portable native C has
 *     no futex, so this file keeps a registry of mutex + condvar
 *     slots keyed by the waited word's address, in exactly the
 *     shape of the weave back-end's cell registry. A real native
 *     LithoSys back-end (Linux futex, Windows WaitOnAddress)
 *     replaces this file whole; nothing in lithothread.c knows
 *     it exists.
 *
 *   - lithosys_clock_now_ns over clock_gettime. The weave
 *     back-end deliberately does not provide the clock (it is
 *     not part of the thread contract), and LithoThread's timed
 *     waits need one on native today. On the wasm host this is
 *     a frozen import the page layer provides; here it is the
 *     host's own clocks, with the frozen numbering mapped at
 *     this boundary and nowhere else.
 *
 * Compiled WITHOUT the LithoRoot include directory (host headers
 * only, LithoRoot headers reached by relative path) so every
 * pthread call below is the host's own, exactly as in the weave
 * back-end. That matters doubly here: this file's registry is
 * built from host mutexes and condvars, and the library under
 * test provides symbols of the same names... the test link keeps
 * the two apart by compiling the library under lt_ prefixed
 * symbols (see docs/LITHOTHREAD.md), so this file's calls are
 * never interposed.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "../include/lithosys.h"
#include "lithothread_wait.h"

/* ---- the wait / notify registry --------------------------------------- */

#define LT__NATIVE_SLOTS 256

typedef struct {
  const volatile uint32_t* addr;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  unsigned waiters;
  int used;
} lt__slot_t;

static lt__slot_t lt__slots[LT__NATIVE_SLOTS];
static pthread_mutex_t lt__slots_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t lt__load(const volatile uint32_t* p) {
  return __atomic_load_n((const uint32_t*)p, __ATOMIC_SEQ_CST);
}

/* Find the slot for an address, creating it when create is set.
 * Returns the slot with its mutex LOCKED, or NULL (creating
 * nothing) when the table is full or no slot exists and create
 * is clear. */
static lt__slot_t* lt__slot_acquire(const volatile uint32_t* addr, int create) {
  int i;
  lt__slot_t* free_slot = NULL;
  pthread_mutex_lock(&lt__slots_mu);
  for (i = 0; i < LT__NATIVE_SLOTS; ++i) {
    lt__slot_t* s = &lt__slots[i];
    if (s->used && s->addr == addr) {
      pthread_mutex_lock(&s->mu);
      pthread_mutex_unlock(&lt__slots_mu);
      return s;
    }
    if (!s->used && free_slot == NULL) free_slot = s;
  }
  if (create && free_slot != NULL) {
    pthread_condattr_t cattr;
    free_slot->used = 1;
    free_slot->addr = addr;
    free_slot->waiters = 0;
    pthread_mutex_init(&free_slot->mu, NULL);
    /* Timed waits compute deadlines on the monotonic clock;
     * the cond must wait on the same clock or the deadline
     * arithmetic is meaningless. */
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&free_slot->cv, &cattr);
    pthread_condattr_destroy(&cattr);
    pthread_mutex_lock(&free_slot->mu);
    pthread_mutex_unlock(&lt__slots_mu);
    return free_slot;
  }
  pthread_mutex_unlock(&lt__slots_mu);
  return NULL;
}

/* Missed-wakeup audit, the same argument as the weave
 * back-end's: the waiter checks the word's value under the slot
 * lock before waiting; the notifier's caller stores the new
 * value BEFORE calling lt__notify, hence before its slot
 * lookup. If the store preceded the waiter's check, the check
 * sees it and the wait never blocks. If it follows, the waiter
 * either holds the slot (the notifier's lookup waits for it,
 * then broadcasts to a thread that is by then blocked in
 * cond_wait, which released the slot to get there) or has not
 * yet checked (and will see the value). No third case. */

static void lt__deadline_from_now(struct timespec* out, int64_t timeout_ns) {
  clock_gettime(CLOCK_MONOTONIC, out);
  out->tv_sec += timeout_ns / 1000000000LL;
  out->tv_nsec += timeout_ns % 1000000000LL;
  if (out->tv_nsec >= 1000000000L) {
    out->tv_sec += 1;
    out->tv_nsec -= 1000000000L;
  }
}

/* The fallback when the slot table is full: poll the word at
 * 1 ms granularity. Bounded and honest: the table has 256 slots
 * and a slot is only held by addresses with live waiters, so
 * test-scale code never reaches this; it exists so a full table
 * degrades instead of deadlocking. */
static int lt__wait_poll(volatile uint32_t* addr, uint32_t expected,
                         int64_t timeout_ns) {
  struct timespec nap;
  int64_t waited = 0;
  nap.tv_sec = 0;
  nap.tv_nsec = 1000000L; /* 1 ms */
  if (lt__load(addr) != expected) return LT_WAIT_NOT_EQUAL;
  for (;;) {
    if (timeout_ns >= 0 && waited >= timeout_ns) return LT_WAIT_TIMED_OUT;
    nanosleep(&nap, NULL);
    waited += 1000000LL;
    if (lt__load(addr) != expected) return LT_WAIT_WOKEN;
  }
}

int lt__wait32(volatile uint32_t* addr, uint32_t expected, int64_t timeout_ns) {
  lt__slot_t* s = lt__slot_acquire(addr, 1);
  struct timespec deadline;
  int have_deadline = 0;
  int result;

  if (s == NULL) return lt__wait_poll(addr, expected, timeout_ns);
  if (lt__load(addr) != expected) {
    pthread_mutex_unlock(&s->mu);
    return LT_WAIT_NOT_EQUAL;
  }
  if (timeout_ns >= 0) {
    if (timeout_ns == 0) {
      pthread_mutex_unlock(&s->mu);
      return LT_WAIT_TIMED_OUT;
    }
    lt__deadline_from_now(&deadline, timeout_ns);
    have_deadline = 1;
  }
  s->waiters++;
  for (;;) {
    int rc = 0;
    if (have_deadline) {
      rc = pthread_cond_timedwait(&s->cv, &s->mu, &deadline);
    } else {
      pthread_cond_wait(&s->cv, &s->mu);
    }
    if (lt__load(addr) != expected) {
      result = LT_WAIT_WOKEN;
      break;
    }
    if (have_deadline && rc == ETIMEDOUT) {
      result = LT_WAIT_TIMED_OUT;
      break;
    }
    /* Spurious wakeup with the value unchanged: keep waiting on
     * the same absolute deadline. */
  }
  s->waiters--;
  pthread_mutex_unlock(&s->mu);
  return result;
}

uint32_t lt__notify(volatile uint32_t* addr, int32_t count) {
  lt__slot_t* s;
  unsigned woken;
  (void)count; /* the registry broadcasts; see the seam header */
  s = lt__slot_acquire(addr, 0);
  if (s == NULL) return 0; /* nobody is waiting on this word */
  woken = s->waiters;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);
  return (uint32_t)woken;
}

/* ---- the LithoSys clock over clock_gettime ------------------------------ */

lithosys_err_t lithosys_clock_now_ns(uint32_t clock_id, uint64_t* out_ns) {
  clockid_t cid;
  struct timespec ts;
  if (out_ns == NULL) return LITHOSYS_EINVAL;
  if (clock_id == LITHOSYS_CLOCK_MONOTONIC) {
    cid = CLOCK_MONOTONIC;
  } else if (clock_id == LITHOSYS_CLOCK_REALTIME) {
    cid = CLOCK_REALTIME;
  } else {
    return LITHOSYS_EINVAL;
  }
  if (clock_gettime(cid, &ts) != 0) return LITHOSYS_EIO;
  *out_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  return LITHOSYS_OK;
}
