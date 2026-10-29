/* lithothread.c ... LithoThread, the pthread-shaped API.
 *
 * Implements include/pthread.h over LithoWeave (spawn, join,
 * exit, the TLS destructor registry) and the LithoSys clock.
 * Synchronization primitives sleep through the value-checked
 * wait seam (lithothread_wait.h): on wasm the wait / notify
 * instructions themselves, provided below under __wasm__; on
 * native hosts the registry back-end in lithothread_native.c.
 * docs/LITHOTHREAD.md is the design note and carries the
 * correctness arguments; this file carries the code.
 *
 * Dependencies: LithoLibC malloc / free (WaferHeap underneath)
 * for records and key value arrays; <setjmp.h> for pthread_exit's
 * unwind to the trampoline pad (the same forward debt the weave
 * carries: in-module setjmp is the toolchain piece's problem,
 * flagged there and here); LithoWeave for everything thread.
 *
 * Error convention: pthread's own. Functions return the error
 * number (LithoLibC's errno values, which Piece 1 and Piece 3
 * arranged to be the same numbers LithoSys returns, so weave and
 * host refusals pass through unmodified) and never write errno.
 */

#include "pthread.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <litholibc/errno_values.h>

#include "lithosys.h"
#include "lithothread_internal.h"

/* The public synchronization structs are ABI (C code allocates
 * them statically): pin their sizes so a layout drift is a build
 * error, not a silent corruption. */
typedef char lt__assert_mutex_size[(sizeof(pthread_mutex_t) == 16) ? 1 : -1];
typedef char lt__assert_cond_size[(sizeof(pthread_cond_t) == 16) ? 1 : -1];
typedef char lt__assert_rwlock_size[(sizeof(pthread_rwlock_t) == 16) ? 1 : -1];
typedef char lt__assert_once_size[(sizeof(pthread_once_t) == 4) ? 1 : -1];

/* Mutex type word poison for a destroyed mutex: any value that
 * is not a flavor makes every entry point answer EINVAL. */
#define LT__TYPE_DESTROYED 0xFFu
/* Cond / rwlock reserved-word poison, same idea. */
#define LT__DESTROYED 0xD15EA5Eu

/* ---- globals --------------------------------------------------------- */
static lt_spin_t lt__registry_lock = LT_SPIN_INIT;
static lt_rec_t* lt__registry; /* live records */
static lt_rec_t* lt__dead;     /* detached records awaiting free */

/* ---- clock helpers -----------------------------------------------------
 * Timed waits take POSIX clock ids (from <time.h>); the frozen
 * host surface numbers its clocks the other way around
 * (LITHOSYS_CLOCK_MONOTONIC is 0, LITHOSYS_CLOCK_REALTIME is 1),
 * so every read goes through this mapping. A failed host read is
 * not expected (both frozen clocks are mandatory); it degrades
 * to 0, which makes a timed wait behave as if no time has passed
 * yet rather than expiring early. */
static uint32_t lt__sys_clock(clockid_t id) {
  return id == CLOCK_MONOTONIC ? LITHOSYS_CLOCK_MONOTONIC
                               : LITHOSYS_CLOCK_REALTIME;
}

static uint64_t lt__now_ns(clockid_t id) {
  uint64_t ns = 0;
  lithosys_err_t err;
  /* Retry on EINTR, mirroring the EINTR discipline stdio's
   * single-iov helpers adopted in pass 1 (audit fix E-5, thread
   * element): an interrupted clock read moved nothing and says
   * nothing about the time, so retry it rather than degrading
   * to 0. The native back-end's clock cannot produce EINTR (its
   * failure answer is EIO), so on this host the loop turns once;
   * hosts that can interrupt get the retry. Any other failure
   * keeps the documented degrade-to-0. */
  do {
    err = lithosys_clock_now_ns(lt__sys_clock(id), &ns);
  } while (err == LITHOSYS_EINTR);
  if (err != LITHOSYS_OK) return 0;
  return ns;
}

/* Validate an abstime and convert it to nanoseconds on its own
 * clock. Returns 0 and fills out_ns, or an errno value. */
static int lt__abstime_ns(const struct timespec* ts, uint64_t* out_ns) {
  if (ts == NULL) return EINVAL;
  if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec > 999999999L) {
    return EINVAL;
  }
  /* Bound tv_sec to the representable nanosecond span: past
   * (UINT64_MAX - tv_nsec) / 1e9 the product wraps in uint64
   * and the deadline lands in the distant past, answering a
   * spurious ETIMEDOUT (and, through lt__remaining_ns, an
   * absurd span converted to int64) for what is really an
   * invalid time (audit fix pass 2, C-F6). EINVAL is this
   * function's verdict for every other malformed field, so the
   * bound rejects rather than clamps. */
  if ((uint64_t)ts->tv_sec >
      (UINT64_MAX - (uint64_t)ts->tv_nsec) / 1000000000ull) {
    return EINVAL;
  }
  *out_ns = (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
  return 0;
}

/* Nanoseconds from now until deadline_ns on clock id, clamped at
 * 0 (a deadline in the past means no wait at all). */
static int64_t lt__remaining_ns(clockid_t id, uint64_t deadline_ns) {
  uint64_t now = lt__now_ns(id);
  if (deadline_ns <= now) return 0;
  return (int64_t)(deadline_ns - now);
}

/* ---- record registry ---------------------------------------------------
 * All list surgery under the registry lock; the lock is never
 * held across a wait, a weave call, a user callback, or a free
 * (frees are collected and done after unlocking). */
static lt_rec_t* lt__find_locked(pthread_t th) {
  lt_rec_t* r;
  for (r = lt__registry; r != NULL; r = r->next) {
    /* Skip records still inside pthread_create: their handle is
     * being written without this lock (review H2), and no caller
     * can hold that handle yet (its thread is parked in the
     * trampoline until creating clears). */
    if (lt_at_load32(&r->creating) != 0u) continue;
    if (r->handle.control == th.control && r->handle.tid == th.tid) return r;
  }
  return NULL;
}

static void lt__unlink_locked(lt_rec_t* rec) {
  lt_rec_t** pp = &lt__registry;
  while (*pp != NULL && *pp != rec) pp = &(*pp)->next;
  if (*pp == rec) *pp = rec->next;
}

static void lt__free_rec(lt_rec_t* rec) {
  free(rec->key_values);
  free(rec);
}

/* Review H1: LithoThread owns the weave lifetime of its spawned
 * threads. Their weave blocks stay JOINABLE at weave level even
 * when the pthread is detached (no lw_detach), so the weave sweep
 * can never free a control block that a queued record's
 * lt__thread_returned still reads. The reaper releases the block
 * itself with lw_join, which returns at once because it is only
 * called after the tid cell has read 0. Exactly one caller reaches
 * this per record (the retire_claimed CAS). */
static void lt__free_detached_rec(lt_rec_t* rec) {
  if (rec->spawned && rec->handle.control != NULL) {
    (void)lw_join(rec->handle, NULL);
  }
  lt__free_rec(rec);
}

/* True once the record's thread has fully returned. The host
 * stores 0 into the weave tid cell only after the whole
 * trampoline has returned, destructor passes included, and that
 * store is the release edge for everything the thread wrote.
 * Until it lands, the exiting thread may still reach its record
 * through lt__current_rec: a key destructor can register
 * further weave destructors whose passes run AFTER the key
 * runner that retired the record (audit fix pass 2, C-F2).
 * While the thread is still in its exit phase its control block
 * is guaranteed alive (the weave sweep needs this same zero
 * cell before it may reap the block), so the read is safe
 * exactly when its answer matters; once the answer is 0 the
 * thread will never touch the record again. The join path
 * satisfies this gate by construction (lw_join returns only
 * after observing the 0); the lazy frees below check it
 * explicitly. The control block's survival is guaranteed by
 * ownership, not timing (review H1): the weave block of a
 * spawned record is never weave-detached, so only
 * lt__free_detached_rec / pthread_join ever release it. */
static int lt__thread_returned(lt_rec_t* rec) {
  const uint32_t* cell = lw_thread_cell(rec->handle);
  return cell == NULL || lt_at_load32(cell) == 0u;
}

/* Free the records the key runners have retired whose threads
 * have fully returned; records whose cells still read nonzero
 * stay queued and the gate is retried at the next reap. Called
 * at the head of pthread_create, pthread_detach and (review L1)
 * pthread_join, and on demand through lithothread_reap_detached():
 * the places that already pay registry costs, which bounds the dead
 * list's lag by thread churn, exactly the weave's lazy-reap
 * bargain. */
static void lt__reap_dead(void) {
  lt_rec_t* dead;
  lt_rec_t* keep = NULL;
  lt_rec_t** keep_tail = &keep;
  lt__spin_lock(&lt__registry_lock);
  dead = lt__dead;
  lt__dead = NULL;
  lt__spin_unlock(&lt__registry_lock);
  while (dead != NULL) {
    lt_rec_t* next = dead->next;
    /* A record whose pthread_create has not returned is not ours
     * to judge: its handle may still be being written (H2). */
    if (lt_at_load32(&dead->creating) == 0u && lt__thread_returned(dead)) {
      lt__free_detached_rec(dead);
    } else {
      dead->next = NULL;
      *keep_tail = dead;
      keep_tail = &dead->next;
    }
    dead = next;
  }
  if (keep != NULL) {
    lt__spin_lock(&lt__registry_lock);
    *keep_tail = lt__dead;
    lt__dead = keep;
    lt__spin_unlock(&lt__registry_lock);
  }
}

/* The key runner's last act for a detached spawned record whose
 * reap it owns (retire_claimed already won by the caller): unlink
 * from the live registry and queue for the lazy free. */
static void lt__retire_rec(lt_rec_t* rec) {
  lt__spin_lock(&lt__registry_lock);
  lt__unlink_locked(rec);
  rec->next = lt__dead;
  lt__dead = rec;
  lt__spin_unlock(&lt__registry_lock);
}

/* ---- key destructor runner ---------------------------------------------
 * The POSIX iteration: repeat passes over the key table; in each
 * pass, every key whose per-thread value is non-NULL and still
 * names this key generation has its value cleared and its
 * destructor called with the value. A destructor may set new
 * values (its own key's included); those are picked up by the
 * next pass. After PTHREAD_DESTRUCTOR_ITERATIONS passes any
 * remaining values are dropped silently, which POSIX permits
 * ("the implementation may stop"). Generation checks make values
 * left by a deleted-and-recreated key inert: their key is gone,
 * so nothing runs for them.
 *
 * This function is the per-record runner registered with the
 * weave at attach / trampoline time, and pthread_exit's foreign
 * path calls it directly. It runs only ever on the owning
 * thread. */
typedef struct {
  void (*dtor)(void*);
  uint32_t gen;
  uint32_t used;
} lt_key_slot_t;

static lt_spin_t lt__key_lock = LT_SPIN_INIT;
static lt_key_slot_t lt__keys[PTHREAD_KEYS_MAX];

static void lt__key_slot_read(pthread_key_t key, lt_key_slot_t* out) {
  out->gen = lt_at_load32(&lt__keys[key].gen);
  out->used = lt_at_load32(&lt__keys[key].used);
  out->dtor = lt__keys[key].dtor;
}

/* Review M2: the destructor runner must see gen, used and dtor as
 * one snapshot. Read unlocked, a pthread_key_delete +
 * pthread_key_create reusing the slot between the gen and dtor
 * reads let an exiting thread match the OLD generation and call
 * the NEW key's destructor on the old value. key_create / delete
 * mutate the slot only under lt__key_lock, so reading under it is
 * a consistent snapshot. Exit path only; the lock is never held
 * across the destructor call itself. */
static void lt__key_slot_read_locked(pthread_key_t key, lt_key_slot_t* out) {
  lt__spin_lock(&lt__key_lock);
  lt__key_slot_read(key, out);
  lt__spin_unlock(&lt__key_lock);
}

void lt__run_key_destructors(lt_rec_t* rec) {
  unsigned pass;
  if (rec == NULL || rec->key_values == NULL) return;
  for (pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; ++pass) {
    int called = 0;
    pthread_key_t k;
    for (k = 0; k < PTHREAD_KEYS_MAX; ++k) {
      void* value = rec->key_values[k].value;
      lt_key_slot_t slot;
      if (value == NULL) continue;
      lt__key_slot_read_locked(k, &slot);
      /* Clear first in every case: a live key's destructor sees
       * a NULL value if it queries its own key, and a dead key's
       * value simply evaporates. */
      rec->key_values[k].value = NULL;
      if (slot.used == 0u || slot.gen != rec->key_values[k].gen ||
          slot.dtor == NULL) {
        continue;
      }
      called = 1;
      slot.dtor(value);
    }
    if (!called) break;
  }
}

/* The weave-registered form: run the destructors, then settle
 * the record's reap. A detached spawned record is retired here
 * if this runner wins the retire claim; a racing pthread_detach
 * that finds exit_phase_done set tries the same claim, and the
 * CAS admits exactly one reaper (the design note walks the
 * interleavings). exit_phase_done is stored BEFORE the detached
 * read for that argument. */
static void lt__key_runner(void* arg) {
  lt_rec_t* rec = (lt_rec_t*)arg;
  /* The exit phase is under way by the time any key destructor
   * can run. The trampoline normally marked it already; this
   * store also covers an entry that exited through lw_exit,
   * which abandoned the trampoline frame before its own mark
   * could execute (audit fix pass 2, C-F3). */
  lt_at_store32(&rec->exit_phase, 1u);
  lt__run_key_destructors(rec);
  lt_at_store32(&rec->exit_phase_done, 1u);
  if (rec->spawned && lt_at_load32(&rec->detached) != 0u &&
      lt_at_cas32(&rec->retire_claimed, 0u, 1u)) {
    lt__retire_rec(rec);
  }
}

/* ---- attach --------------------------------------------------------------
 * A recursion guard on attach: attach allocates, and allocation
 * failure writes errno through an accessor that itself wants to
 * attach. The guard makes the nested attempt answer NULL (the
 * accessor then falls back to its plain slot) instead of
 * recursing to exhaustion. */
static LT__THREAD_LOCAL int lt__attaching;
static void lt__once_abandon_all(void); /* review M4, see "once" */

lt_rec_t* lt__attach_rec(void) {
  lt_rec_t* rec;
  if (lt__current_rec != NULL) return lt__current_rec;
  if (lt__attaching) return NULL;
  lt__attaching = 1;
  rec = (lt_rec_t*)calloc(1, sizeof(lt_rec_t));
  if (rec != NULL) {
    rec->spawned = 0;
    rec->handle = lw_self(); /* attaches weave-side as well */
    lt__spin_lock(&lt__registry_lock);
    rec->next = lt__registry;
    lt__registry = rec;
    lt__spin_unlock(&lt__registry_lock);
    lt__current_rec = rec;
    /* If this registration fails (out of memory) the record
     * still serves TLS and errno; only its key destructors are
     * lost, a degraded corner documented in the note. */
    (void)lw_thread_atexit(lt__key_runner, rec);
  }
  lt__attaching = 0;
  return rec;
}

/* Review L1: reaping is lazy, so the last detached threads' records
 * would otherwise stay allocated until the next create / join /
 * detach. Shutdown paths and leak tools call this to collect every
 * record whose thread has fully returned. */
void lithothread_reap_detached(void) { lt__reap_dead(); }

void lithothread_release_self(void) {
  lt_rec_t* rec = lt__current_rec;
  if (rec != NULL && rec->spawned) return;
  if (rec != NULL) {
    if (lw_thread_tid(rec->handle) == 1) return; /* main thread */
    /* Destructors first, with the record alive: the weave list
     * holds lt__key_runner(rec). */
    lw_run_tls_destructors();
    lt__run_key_destructors(rec);
    lt__spin_lock(&lt__registry_lock);
    lt__unlink_locked(rec);
    lt__spin_unlock(&lt__registry_lock);
    lt__current_rec = NULL;
    lt__free_rec(rec);
  }
  lw_release_self();
}

lt_rec_t* lt__self_rec(void) {
  if (lt__current_rec != NULL) return lt__current_rec;
  return lt__attach_rec();
}

/* ---- thread entry trampoline -------------------------------------------
 * pthread_create's weave entry. Copies the start pack, frees it,
 * adopts the record prepared by the creator, registers the key
 * runner with the weave (so key destructors run inside the
 * weave's exit phase, after the entry returns), and runs the
 * user routine under the pthread_exit landing pad. */
typedef struct {
  void* (*start)(void*);
  void* arg;
  lt_rec_t* rec;
} lt_start_pack_t;

static void* lt__trampoline(void* p) {
  lt_start_pack_t* pack = (lt_start_pack_t*)p;
  lt_rec_t* rec = pack->rec;
  void* (*start)(void*) = pack->start;
  void* arg = pack->arg;
  free(pack);

  /* Review H2: do not run user code (or reach the exit path that
   * may retire the record) until the creator has finished
   * publishing rec->handle. The window is the tail of
   * lithosys_thread_spawn on the creator, so a pause loop. */
  while (lt_at_load32(&rec->creating) != 0u) lt_at_spin_pause();

  lt__current_rec = rec;
  (void)lw_thread_atexit(lt__key_runner, rec);
  if (setjmp(rec->exit_pad) == 0) {
    rec->retval = start(arg);
  } else {
    rec->retval = rec->exit_value;
  }
  /* The exit phase begins here: this frame is about to return
   * to the weave, which runs the destructor passes, and the
   * landing pad above dies with the frame. Mark it so a
   * pthread_exit called from a destructor takes the delegation
   * path in pthread_exit instead of longjmping into this dead
   * frame (audit fix pass 2, C-F3). */
  lt_at_store32(&rec->exit_phase, 1u);
  return rec->retval;
}

/* ---- threads ------------------------------------------------------------- */

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg) {
  int detached = 0;
  size_t stacksize = 0;
  lt_rec_t* rec;
  lt_start_pack_t* pack;
  lw_descriptor_t desc;
  lithosys_err_t err;

  if (thread == NULL || start_routine == NULL) return EINVAL;
  if (attr != NULL) {
    if (attr->detachstate != (uint32_t)PTHREAD_CREATE_JOINABLE &&
        attr->detachstate != (uint32_t)PTHREAD_CREATE_DETACHED) {
      return EINVAL;
    }
    detached = attr->detachstate == (uint32_t)PTHREAD_CREATE_DETACHED;
    stacksize = attr->stacksize;
  }
  lt__reap_dead();

  rec = (lt_rec_t*)calloc(1, sizeof(lt_rec_t));
  if (rec == NULL) return EAGAIN;
  pack = (lt_start_pack_t*)malloc(sizeof(lt_start_pack_t));
  if (pack == NULL) {
    free(rec);
    return EAGAIN;
  }
  rec->spawned = 1;
  lt_at_store32(&rec->detached, detached ? 1u : 0u);
  /* Review H2: the creator's claim. The child cannot run user
   * code, and no reaper or lookup will touch the record, until
   * this clears after *thread is written. */
  lt_at_store32(&rec->creating, 1u);
  pack->start = start_routine;
  pack->arg = arg;
  pack->rec = rec;

  lt__spin_lock(&lt__registry_lock);
  rec->next = lt__registry;
  lt__registry = rec;
  lt__spin_unlock(&lt__registry_lock);

  desc.entry = lt__trampoline;
  desc.arg = pack;
  desc.stack_size = stacksize;
  desc.name = NULL;
  /* No weave-level detach for a detached pthread (review H1): the
   * weave block stays joinable and the record's reaper releases it
   * with lw_join once the thread has returned. */
  err = lw_spawn(&desc, &rec->handle);
  if (err != LITHOSYS_OK) {
    /* The weave retires its own control block on a refused
     * spawn; ours is unwound the same total way. The host's code
     * (EAGAIN / ENOMEM / EINVAL) is already in our numbering. */
    lt__spin_lock(&lt__registry_lock);
    lt__unlink_locked(rec);
    lt__spin_unlock(&lt__registry_lock);
    free(pack);
    free(rec);
    return (int)err;
  }
  {
    /* Copy out first: once creating clears, a detached record may
     * be retired and freed at any moment (review H2). */
    pthread_t h = rec->handle;
    *thread = h;
    lt_at_store32(&rec->creating, 0u);
  }
  return 0;
}

int pthread_join(pthread_t thread, void** retval) {
  lt_rec_t* rec;
  void* rv = NULL;
  lithosys_err_t err;

  if (pthread_equal(thread, pthread_self())) return EDEADLK;
  lt__reap_dead(); /* review L1: join pays the reap too */
  lt__spin_lock(&lt__registry_lock);
  rec = lt__find_locked(thread);
  if (rec == NULL) {
    lt__spin_unlock(&lt__registry_lock);
    return ESRCH;
  }
  if (lt_at_load32(&rec->detached) != 0u || rec->join_claimed) {
    lt__spin_unlock(&lt__registry_lock);
    return EINVAL;
  }
  rec->join_claimed = 1;
  lt__spin_unlock(&lt__registry_lock);

  err = lw_join(thread, &rv);
  if (err != LITHOSYS_OK) {
    /* Our registry is authoritative for detach / claim state, so
     * a refusal here means the weave disagrees about the handle
     * itself. Unreachable through this API's own calls. Review
     * L11: drop the claim so the record is not left permanently
     * unjoinable and undetachable; nothing was unlinked or freed,
     * so releasing it cannot double-reap. */
    lt__spin_lock(&lt__registry_lock);
    rec->join_claimed = 0;
    lt__spin_unlock(&lt__registry_lock);
    return EINVAL;
  }
  /* Answer the value the weave collected: it is the only write
   * that survives every exit path. A thread that returned or
   * pthread_exit'd through the trampoline also stored it in the
   * record, but a thread that exited through lw_exit bypassed
   * the trampoline entirely and its value lives only in the
   * weave control block (audit fix pass 2, C-F4). The one
   * exception is a pthread_exit called after the exit phase
   * began: the trampoline had already frozen the weave's value
   * when it returned, so the late value rides the record's
   * override instead (C-F3). lw_join observed the tid cell's
   * zero store, whose release edge orders both reads. The free
   * below is the reap the C-F2 gate describes: lw_join's return
   * already proves the cell read 0. */
  lt__spin_lock(&lt__registry_lock);
  lt__unlink_locked(rec);
  lt__spin_unlock(&lt__registry_lock);
  if (retval != NULL) {
    *retval = lt_at_load32(&rec->exit_override) != 0u ? rec->exit_value
                                                      : rv;
  }
  lt__free_rec(rec);
  return 0;
}

int pthread_detach(pthread_t thread) {
  lt_rec_t* rec;

  lt__reap_dead();
  lt__spin_lock(&lt__registry_lock);
  rec = lt__find_locked(thread);
  if (rec == NULL) {
    lt__spin_unlock(&lt__registry_lock);
    return ESRCH;
  }
  if (!rec->spawned || lt_at_load32(&rec->detached) != 0u ||
      rec->join_claimed) {
    /* An attached (main / foreign) thread was never joinable; the
     * weave refused its detach before review H1, and still does
     * by this check. */
    lt__spin_unlock(&lt__registry_lock);
    return EINVAL;
  }
  lt_at_store32(&rec->detached, 1u);
  lt__spin_unlock(&lt__registry_lock);

  /* If the thread already finished, its key runner has run (it
   * precedes the host's completion store, which precedes any
   * observation of completion) and stored exit_phase_done. Win
   * the retire claim and the reap is ours; lose it and the
   * runner already queued the record. Either way exactly one
   * reaper exists. But runner-done is not thread-done (C-F2):
   * the free also waits for the tid cell to read 0, and a
   * record whose thread is still in its destructor phase is
   * queued for the lazy reap instead of freed here. */
  if (lt_at_load32(&rec->exit_phase_done) != 0u &&
      lt_at_cas32(&rec->retire_claimed, 0u, 1u)) {
    if (lt__thread_returned(rec)) {
      lt__spin_lock(&lt__registry_lock);
      lt__unlink_locked(rec);
      lt__spin_unlock(&lt__registry_lock);
      lt__free_detached_rec(rec);
    } else {
      lt__retire_rec(rec);
    }
  }
  return 0;
}

pthread_t pthread_self(void) {
  pthread_t self = lw_self();
  return self;
}

int pthread_equal(pthread_t a, pthread_t b) {
  return a.control == b.control && a.tid == b.tid;
}

void pthread_exit(void* retval) {
  lt_rec_t* rec = lt__current_rec;
  lt__once_abandon_all(); /* review M4 */
  if (rec != NULL && rec->spawned) {
    if (lt_at_load32(&rec->exit_phase) == 0u) {
      rec->exit_value = retval;
      longjmp(rec->exit_pad, 1);
    }
    /* The trampoline has already returned: this call comes from
     * inside the weave's destructor phase (a key destructor, or
     * a TLS destructor registered with the weave directly), and
     * the landing pad's frame no longer exists. POSIX leaves
     * pthread_exit from a destructor undefined; the safe failure
     * mode is to record the value where the joiner will find it
     * (the override pthread_join answers) and delegate the exit
     * to the weave, whose in-phase lw_exit abandons the
     * remaining destructors and completes the exit already in
     * progress (audit fix pass 2, C-F3). */
    rec->exit_value = retval;
    lt_at_store32(&rec->exit_override, 1u);
    /* The weave's in-phase exit abandons the key runner along
     * with the remaining destructors, so a detached record would
     * never be retired: do the runner's retirement here, under
     * the same claim, before delegating. A joinable record is
     * left for its joiner, as always. */
    if (lt_at_load32(&rec->detached) != 0u &&
        lt_at_cas32(&rec->retire_claimed, 0u, 1u)) {
      lt_at_store32(&rec->exit_phase_done, 1u);
      lt__retire_rec(rec);
    }
    lw_exit(retval);
    for (;;) {
      /* lw_exit never returns in this phase either. */
    }
  }
  /* Main, foreign, or weave-spawned-but-not-ours thread: run any
   * key destructors this thread accumulated, then hand the exit
   * to the weave, whose own exit runs its destructor registry
   * (our runner included, a no-op second pass over nulled
   * values) and ends a main thread's instance. That main-thread
   * meaning is a documented simplification of POSIX, which would
   * keep the process alive for its remaining threads; the
   * instance model has no such suspended state. */
  if (rec != NULL) lt__run_key_destructors(rec);
  lw_exit(retval);
  for (;;) {
    /* lw_exit never returns; this satisfies compilers that
     * cannot see through the weave. */
  }
}

/* ---- thread attributes ----------------------------------------------------- */

int pthread_attr_init(pthread_attr_t* attr) {
  if (attr == NULL) return EINVAL;
  attr->detachstate = (uint32_t)PTHREAD_CREATE_JOINABLE;
  attr->reserved = 0;
  attr->stacksize = 0;
  return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
  if (attr == NULL) return EINVAL;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int detachstate) {
  if (attr == NULL) return EINVAL;
  if (detachstate != PTHREAD_CREATE_JOINABLE &&
      detachstate != PTHREAD_CREATE_DETACHED) {
    return EINVAL;
  }
  attr->detachstate = (uint32_t)detachstate;
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t* attr, int* detachstate) {
  if (attr == NULL || detachstate == NULL) return EINVAL;
  *detachstate = (int)attr->detachstate;
  return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, size_t stacksize) {
  if (attr == NULL) return EINVAL;
  attr->stacksize = stacksize;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t* attr, size_t* stacksize) {
  if (attr == NULL || stacksize == NULL) return EINVAL;
  *stacksize = attr->stacksize;
  return 0;
}

/* ---- mutexes -----------------------------------------------------------------
 * The word protocol is the classic three-state futex mutex
 * (Drepper, "Futexes Are Tricky"): state 0 unlocked, 1 locked
 * with no waiters marked, 2 locked with waiters possibly asleep.
 * A locker that finds the word held exchanges it to 2 (marking
 * contention) and sleeps on the word expecting 2; the unlocker
 * decrements, and only when the old value was 2 does it store 0
 * and notify one sleeper. Every state transition the protocol
 * needs is a single atomic operation, and the wait is
 * value-checked by the seam, so the exchange-to-2 before
 * sleeping is what makes a wakeup un-losable: an unlock racing
 * the entry to sleep either decrements first (the sleeper's wait
 * observes the changed word) or notifies after (the sleeper is
 * blocked where the notify lands). The spin before sleeping is
 * deliberately zero: callers that want spin-then-sleep compose
 * trylock loops, and an unbounded in-library spin is how a
 * low-priority holder gets starved by its own waiters on a
 * cooperative host. (The short test-and-test-and-set spin in the
 * registry lock is a different animal: its critical sections are
 * a handful of instructions and never wait.)
 *
 * Flavor logic (owner / count) wraps the word protocol for the
 * errorcheck and recursive types. The normal type never touches
 * owner, which keeps the hot path at one CAS uncontended. */

/* Acquire the word. abstime NULL waits indefinitely; otherwise
 * the deadline is on CLOCK_REALTIME (POSIX mutex timedlock
 * semantics) and expiry answers ETIMEDOUT with the word left in
 * state 2 (still owned by its holder, contention still marked),
 * which the holder's unlock handles by its ordinary path. */
static int lt__mutex_word_lock(pthread_mutex_t* m,
                               const struct timespec* abstime) {
  uint64_t deadline = 0;
  if (abstime != NULL) {
    int rc = lt__abstime_ns(abstime, &deadline);
    if (rc != 0) return rc;
  }
  if (lt_at_cas32(&m->state, 0u, 1u)) return 0;
  for (;;) {
    uint32_t c = lt_at_exchange32(&m->state, 2u);
    if (c == 0u) return 0; /* was unlocked; we hold it, marked 2 */
    /* c is 1 or 2: held. Sleep while the word reads 2. */
    {
      int64_t tmo = -1;
      int wrc;
      if (abstime != NULL) {
        tmo = lt__remaining_ns(CLOCK_REALTIME, deadline);
        if (tmo <= 0) return ETIMEDOUT;
      }
      wrc = lt__wait32(&m->state, 2u, tmo);
      if (abstime != NULL && wrc == LT_WAIT_TIMED_OUT) return ETIMEDOUT;
    }
  }
}

/* Release the word. The caller has established it holds the
 * mutex (flavor checks live in the wrappers). */
static void lt__mutex_word_unlock(pthread_mutex_t* m) {
  LT_TSAN_RELEASE(m);
  if (lt_at_fetch_sub32(&m->state, 1u) != 1u) {
    /* Old value was 2: waiters may be asleep on the word. */
    lt_at_store32(&m->state, 0u);
    lt__notify(&m->state, 1);
  }
}

static uint32_t lt__mutex_type(pthread_mutex_t* m) {
  return lt_at_load32(&m->type);
}

/* Validate the type word; returns the flavor or -1 destroyed. */
static int lt__mutex_flavor(pthread_mutex_t* m) {
  uint32_t t = lt__mutex_type(m);
  if (t == LT__TYPE_DESTROYED) return -1;
  if (t > (uint32_t)PTHREAD_MUTEX_ERRORCHECK) return -1;
  return (int)t;
}

/* Recursive relock counting. Only the owning thread ever
 * touches count (it holds the word), so a plain load / store
 * suffices. At UINT32_MAX the relock is refused with EAGAIN
 * (POSIX: recursive lock count exceeded) instead of wrapping to 0,
 * which the unlock path would misread as the final release (audit
 * fix pass 2, C-F5), and instead of silently saturating, which
 * left one unlock unbalanced (review L4). */
static int lt__count_inc(pthread_mutex_t* m) {
  uint32_t c = lt_at_load32(&m->count);
  if (c == UINT32_MAX) return EAGAIN;
  lt_at_store32(&m->count, c + 1u);
  return 0;
}

static int lt__mutex_lock_common(pthread_mutex_t* m,
                                 const struct timespec* abstime) {
  int flavor;
  uint32_t self = 0;
  int rc;

  if (m == NULL) return EINVAL;
  flavor = lt__mutex_flavor(m);
  if (flavor < 0) return EINVAL;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    self = (uint32_t)lw_current_tid();
    if (lt_at_load32(&m->state) != 0u &&
        lt_at_load32(&m->owner) == self) {
      if (flavor == PTHREAD_MUTEX_RECURSIVE) return lt__count_inc(m);
      if (abstime == NULL) return EDEADLK;
      /* A timed lock on a mutex the caller already holds: the
       * deadline can never rescue a self-deadlock, and POSIX
       * timedlock answers ETIMEDOUT only for time expiry; the
       * errorcheck answer stays EDEADLK. */
      return EDEADLK;
    }
  }
  rc = lt__mutex_word_lock(m, abstime);
  if (rc != 0) return rc;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    lt_at_store32(&m->owner, self);
    lt_at_store32(&m->count, 1u);
  }
  LT_TSAN_ACQUIRE(m);
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
  return lt__mutex_lock_common(m, NULL);
}

int pthread_mutex_timedlock(pthread_mutex_t* m,
                            const struct timespec* abstime) {
  if (abstime == NULL) return EINVAL;
  return lt__mutex_lock_common(m, abstime);
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
  int flavor;
  uint32_t self = 0;

  if (m == NULL) return EINVAL;
  flavor = lt__mutex_flavor(m);
  if (flavor < 0) return EINVAL;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    self = (uint32_t)lw_current_tid();
    if (lt_at_load32(&m->state) != 0u &&
        lt_at_load32(&m->owner) == self) {
      if (flavor == PTHREAD_MUTEX_RECURSIVE) return lt__count_inc(m);
      return EBUSY; /* trylock never deadlocks, even errorcheck */
    }
  }
  if (!lt_at_cas32(&m->state, 0u, 1u)) return EBUSY;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    lt_at_store32(&m->owner, self);
    lt_at_store32(&m->count, 1u);
  }
  LT_TSAN_ACQUIRE(m);
  return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
  int flavor;

  if (m == NULL) return EINVAL;
  flavor = lt__mutex_flavor(m);
  if (flavor < 0) return EINVAL;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    uint32_t self = (uint32_t)lw_current_tid();
    if (lt_at_load32(&m->state) == 0u ||
        lt_at_load32(&m->owner) != self) {
      return EPERM;
    }
    if (flavor == PTHREAD_MUTEX_RECURSIVE) {
      uint32_t count = lt_at_load32(&m->count);
      if (count > 1u) {
        lt_at_store32(&m->count, count - 1u);
        return 0;
      }
    }
    /* Clear ownership BEFORE releasing the word: a new acquirer
     * writes its own owner the moment it holds the word, and a
     * late clear from us would erase it. */
    lt_at_store32(&m->count, 0u);
    lt_at_store32(&m->owner, 0u);
  } else if (lt_at_load32(&m->state) == 0u) {
    /* Unlocking an unlocked normal mutex is undefined in POSIX;
     * we answer EINVAL rather than corrupting the word the way
     * a blind decrement would (it would take state to a huge
     * value and wedge the mutex forever). */
    return EINVAL;
  }
  lt__mutex_word_unlock(m);
  return 0;
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr) {
  int type = PTHREAD_MUTEX_NORMAL;
  if (m == NULL) return EINVAL;
  if (attr != NULL) {
    if (attr->type < PTHREAD_MUTEX_NORMAL ||
        attr->type > PTHREAD_MUTEX_ERRORCHECK) {
      return EINVAL;
    }
    type = attr->type;
  }
  lt_at_store32(&m->state, 0u);
  lt_at_store32(&m->type, (uint32_t)type);
  lt_at_store32(&m->owner, 0u);
  lt_at_store32(&m->count, 0u);
  return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
  if (m == NULL) return EINVAL;
  if (lt__mutex_flavor(m) < 0) return EINVAL;
  if (lt_at_load32(&m->state) != 0u) return EBUSY;
  lt_at_store32(&m->type, LT__TYPE_DESTROYED);
  return 0;
}

/* ---- mutex attributes ------------------------------------------------------- */

int pthread_mutexattr_init(pthread_mutexattr_t* attr) {
  if (attr == NULL) return EINVAL;
  attr->type = PTHREAD_MUTEX_DEFAULT;
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t* attr) {
  if (attr == NULL) return EINVAL;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
  if (attr == NULL) return EINVAL;
  if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
    return EINVAL;
  }
  attr->type = type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t* attr, int* type) {
  if (attr == NULL || type == NULL) return EINVAL;
  *type = attr->type;
  return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t* attr, int pshared) {
  (void)attr;
  if (pshared == PTHREAD_PROCESS_PRIVATE) return 0;
  if (pshared == PTHREAD_PROCESS_SHARED) return ENOTSUP;
  return EINVAL;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t* attr,
                                 int* pshared) {
  if (attr == NULL || pshared == NULL) return EINVAL;
  *pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

/* ---- condition variables -----------------------------------------------------
 * Construction: a sequence word and a waiter count, nothing
 * else. The waiter's protocol, executed while holding the user
 * mutex:
 *
 *   1. waiters += 1 (atomic);
 *   2. snap = seq (atomic load);
 *   3. release the mutex;
 *   4. sleep on seq's address with the seam, expecting snap;
 *   5. when seq != snap (or the deadline passes), re-acquire
 *      the mutex and return.
 *
 * The signaler changes the predicate under the same mutex, then
 * (holding it or not) bumps seq and, when waiters read non-zero,
 * notifies seq's address.
 *
 * Why no wakeup can be missed: the waiter's snapshot (step 2)
 * precedes its mutex release (step 3), and the mutex orders
 * critical sections, so consider where the signaler's bump can
 * land. If the signaler's predicate change ran BEFORE the
 * waiter's critical section, the waiter's own predicate check
 * (also under the mutex, before step 1) already saw the new
 * predicate and it never waits. If it ran AFTER, the bump is
 * ordered after the snapshot by the mutex handoff, so the
 * seam's value check in step 4 observes seq != snap and refuses
 * to block (or the notify reaches the blocked waiter). There is
 * no third interleaving, and that is the entire argument: the
 * futex-style value-checked wait is what makes a bump that
 * lands between snapshot and sleep harmless.
 *
 * Signal wakes ALL current waiters (a herd), not one. With a
 * bare sequence word there is no honest wake-one: the seam
 * might pick a waiter whose predicate is still false, it would
 * re-sleep, and the waiter whose predicate the signal meant
 * would stay asleep with its wakeup spent... a missed wakeup
 * wearing a costume. Waking everyone costs scheduler churn in
 * exchange for a proof with no holes, and LithoThread's waiter
 * counts (engine threads handing off queues) are small.
 * Broadcast is the same code path.
 *
 * The mutex used with a cond may be recursive here: wait
 * releases it FULLY and restores the recursion count on the way
 * back, a definite semantics where POSIX leaves the behavior
 * undefined. Errorcheck and recursive mutexes are ownership
 * checked (EINVAL when the caller does not hold the mutex); a
 * normal mutex cannot be checked, as everywhere. */

static int lt__cond_validate(pthread_cond_t* c) {
  if (c == NULL) return EINVAL;
  if (lt_at_load32(&c->reserved) == LT__DESTROYED) return EINVAL;
  return 0;
}

/* Release the caller's mutex completely for a cond wait;
 * returns the recursion level to restore (1 for non-recursive
 * flavors). The caller has validated ownership. */
static uint32_t lt__cond_release_mutex(pthread_mutex_t* m) {
  uint32_t levels = 1;
  if (lt__mutex_type(m) == (uint32_t)PTHREAD_MUTEX_RECURSIVE) {
    levels = lt_at_load32(&m->count);
    lt_at_store32(&m->count, 0u);
    lt_at_store32(&m->owner, 0u);
  } else if (lt__mutex_type(m) == (uint32_t)PTHREAD_MUTEX_ERRORCHECK) {
    lt_at_store32(&m->owner, 0u);
  }
  lt__mutex_word_unlock(m);
  return levels;
}

/* Re-acquire after a cond wait and restore flavor state. The
 * word wait is indefinite: re-acquisition is not timed, exactly
 * as POSIX specifies for the re-acquire half of a timed wait
 * (the deadline governs the sleep, never the re-acquire, so a
 * timed wait can return late but never wrongly). */
static void lt__cond_reacquire_mutex(pthread_mutex_t* m, uint32_t levels) {
  (void)lt__mutex_word_lock(m, NULL);
  if (lt__mutex_type(m) == (uint32_t)PTHREAD_MUTEX_RECURSIVE) {
    lt_at_store32(&m->owner, (uint32_t)lw_current_tid());
    lt_at_store32(&m->count, levels);
  } else if (lt__mutex_type(m) == (uint32_t)PTHREAD_MUTEX_ERRORCHECK) {
    lt_at_store32(&m->owner, (uint32_t)lw_current_tid());
  }
  LT_TSAN_ACQUIRE(m);
}

static int lt__cond_wait_common(pthread_cond_t* c, pthread_mutex_t* m,
                                const struct timespec* abstime) {
  int flavor;
  uint64_t deadline = 0;
  uint32_t snap;
  uint32_t levels;
  int result = 0;

  if (lt__cond_validate(c) != 0 || m == NULL) return EINVAL;
  flavor = lt__mutex_flavor(m);
  if (flavor < 0) return EINVAL;
  if (flavor != PTHREAD_MUTEX_NORMAL) {
    if (lt_at_load32(&m->state) == 0u ||
        lt_at_load32(&m->owner) != (uint32_t)lw_current_tid()) {
      return EINVAL; /* waiting on a mutex the caller does not hold */
    }
  }
  if (abstime != NULL) {
    int rc = lt__abstime_ns(abstime, &deadline);
    if (rc != 0) return rc;
  }

  /* Registration and snapshot, both before the mutex release
   * (the proof in the section header needs this order). */
  lt_at_fetch_add32(&c->waiters, 1u);
  snap = lt_at_load32(&c->seq);
  levels = lt__cond_release_mutex(m);

  for (;;) {
    int64_t tmo = -1;
    int wrc;
    if (abstime != NULL) {
      tmo = lt__remaining_ns((clockid_t)c->clockid, deadline);
      if (tmo <= 0) {
        result = (lt_at_load32(&c->seq) != snap) ? 0 : ETIMEDOUT;
        break;
      }
    }
    wrc = lt__wait32(&c->seq, snap, tmo);
    if (lt_at_load32(&c->seq) != snap) {
      result = 0; /* a signal or broadcast landed during our sleep */
      break;
    }
    if (abstime != NULL && wrc == LT_WAIT_TIMED_OUT) {
      result = ETIMEDOUT;
      break;
    }
    /* Otherwise a spurious return: keep sleeping. */
  }

  lt_at_fetch_sub32(&c->waiters, 1u);
  lt__cond_reacquire_mutex(m, levels);
  LT_TSAN_ACQUIRE(c);
  return result;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  return lt__cond_wait_common(c, m, NULL);
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* abstime) {
  if (abstime == NULL) return EINVAL;
  return lt__cond_wait_common(c, m, abstime);
}

static int lt__cond_wake(pthread_cond_t* c) {
  if (lt__cond_validate(c) != 0) return EINVAL;
  lt_at_fetch_add32(&c->seq, 1u);
  LT_TSAN_RELEASE(c);
  if (lt_at_load32(&c->waiters) != 0u) {
    lt__notify(&c->seq, -1); /* the herd, by design (see header) */
  }
  return 0;
}

int pthread_cond_signal(pthread_cond_t* c) { return lt__cond_wake(c); }

int pthread_cond_broadcast(pthread_cond_t* c) { return lt__cond_wake(c); }

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr) {
  clockid_t clockid = CLOCK_REALTIME;
  if (c == NULL) return EINVAL;
  if (attr != NULL) {
    if (attr->pshared == PTHREAD_PROCESS_SHARED) return ENOTSUP;
    if (attr->pshared != PTHREAD_PROCESS_PRIVATE) return EINVAL;
    if (attr->clockid != CLOCK_REALTIME && attr->clockid != CLOCK_MONOTONIC) {
      return EINVAL;
    }
    clockid = attr->clockid;
  }
  lt_at_store32(&c->seq, 0u);
  lt_at_store32(&c->waiters, 0u);
  c->clockid = (int32_t)clockid;
  lt_at_store32(&c->reserved, 0u);
  return 0;
}

int pthread_cond_destroy(pthread_cond_t* c) {
  if (lt__cond_validate(c) != 0) return EINVAL;
  if (lt_at_load32(&c->waiters) != 0u) return EBUSY;
  lt_at_store32(&c->reserved, LT__DESTROYED);
  return 0;
}

/* ---- cond attributes ------------------------------------------------------------ */

int pthread_condattr_init(pthread_condattr_t* attr) {
  if (attr == NULL) return EINVAL;
  attr->clockid = (int32_t)CLOCK_REALTIME;
  attr->pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t* attr) {
  if (attr == NULL) return EINVAL;
  return 0;
}

int pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t clock_id) {
  if (attr == NULL) return EINVAL;
  if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
    return EINVAL;
  }
  attr->clockid = (int32_t)clock_id;
  return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t* attr,
                              clockid_t* clock_id) {
  if (attr == NULL || clock_id == NULL) return EINVAL;
  *clock_id = (clockid_t)attr->clockid;
  return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t* attr, int pshared) {
  if (attr == NULL) return EINVAL;
  if (pshared != PTHREAD_PROCESS_PRIVATE &&
      pshared != PTHREAD_PROCESS_SHARED) {
    return EINVAL;
  }
  attr->pshared = pshared;
  return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t* attr, int* pshared) {
  if (attr == NULL || pshared == NULL) return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

/* ---- read / write locks ---------------------------------------------------------
 * One word: bit 31 marks a holding writer, the low bits count
 * readers. Two sequence words carry the sleeps: readers sleep on
 * rseq (bumped by a releasing writer), writers on wseq (bumped by
 * a releasing writer and by the last departing reader). Every
 * sleep is the seam's value-checked wait against a snapshot, the
 * same no-missed-wakeup shape as the condvar: state that would
 * release a sleeper is stored before the sequence bump, and the
 * bump before the notify, so a sleeper either observes the state
 * or is woken.
 *
 * Reader-preference is deliberate and documented: a reader that
 * finds no writer holding barges in ahead of waiting writers.
 * The cost is writer starvation under a sustained read flood;
 * the benefit is that read-mostly engine tables never convoy
 * behind a queued writer. POSIX permits either preference.
 * Upgrading a held read lock to a write lock is undefined (it
 * deadlocks here, as in most implementations); release first.
 *
 * Review L5 notes. The reader count is capped one below the writer
 * bit: rdlock / tryrdlock at the cap answer EAGAIN (POSIX: maximum
 * read locks exceeded) instead of carrying into bit 31. Unlock does
 * not check ownership: the 16-byte layout has no room for a writer
 * tid, so unlocking a lock the caller does not hold is undefined
 * behaviour (as POSIX states) and is not diagnosed. Writer
 * starvation under a read flood is the documented price of reader
 * preference above, and a writer release wakes every sleeper of
 * both classes (a herd), as pthread_cond_signal does (see
 * lt__cond_wake). */

#define LT__RW_WRITER 0x80000000u
#define LT__RW_READERS 0x7FFFFFFFu

static int lt__rwlock_validate(pthread_rwlock_t* r) {
  if (r == NULL) return EINVAL;
  if (lt_at_load32(&r->reserved) == LT__DESTROYED) return EINVAL;
  return 0;
}

/* Shared acquisition core. write != 0 wants the writer bit;
 * abstime NULL waits indefinitely (deadline on CLOCK_REALTIME,
 * POSIX rwlock timed semantics). */
static int lt__rwlock_acquire(pthread_rwlock_t* r, int write,
                              const struct timespec* abstime) {
  uint64_t deadline = 0;
  if (abstime != NULL) {
    int rc = lt__abstime_ns(abstime, &deadline);
    if (rc != 0) return rc;
  }
  for (;;) {
    /* Snapshot the sequence word BEFORE attempting the lock
     * word (audit fix pass 2, C-F1). The release side stores the
     * freed lock word, then bumps the sequence, then notifies,
     * in that order. With the snapshot first, every release is
     * accounted for: if it completed before our snapshot, the
     * attempt below observes the freed word (the release's lock
     * store precedes its bump, and our lock load follows our
     * snapshot); if it lands after our snapshot, it bumps the
     * sequence past our snapshot, so the wait's value check
     * refuses to block (or its notify reaches us blocked). The
     * old order (attempt, then snapshot) inverted the first
     * half: a release landing between a failed attempt and the
     * snapshot left the snapshot holding the post-bump value
     * while the lock stood free, and the wait slept a whole
     * deadline on a free lock... the missed wakeup the
     * condvar's snapshot-before-release order exists to
     * prevent. This ordering is that proof, transplanted. */
    volatile uint32_t* seqp = write ? &r->wseq : &r->rseq;
    uint32_t snap = lt_at_load32(seqp);
    uint32_t l = lt_at_load32(&r->lock);
    if (write) {
      if (l == 0u && lt_at_cas32(&r->lock, 0u, LT__RW_WRITER)) break;
    } else {
      if ((l & LT__RW_WRITER) == 0u && l == LT__RW_READERS) return EAGAIN;
      if ((l & LT__RW_WRITER) == 0u &&
          lt_at_cas32(&r->lock, l, l + 1u)) {
        break;
      }
    }
    /* Someone holds what we need: sleep on our class's sequence
     * word expecting the snapshot. A passed deadline is answered
     * here, before sleeping: the attempt above was the final
     * non-blocking chance (the holder may have just released),
     * which keeps the old code's honest boundary. */
    {
      int64_t tmo = -1;
      if (abstime != NULL) {
        tmo = lt__remaining_ns(CLOCK_REALTIME, deadline);
        if (tmo <= 0) return ETIMEDOUT;
      }
      (void)lt__wait32(seqp, snap, tmo);
    }
  }
  LT_TSAN_ACQUIRE(r);
  return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* r) {
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  return lt__rwlock_acquire(r, 0, NULL);
}

int pthread_rwlock_wrlock(pthread_rwlock_t* r) {
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  return lt__rwlock_acquire(r, 1, NULL);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t* r,
                               const struct timespec* abstime) {
  if (abstime == NULL) return EINVAL;
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  return lt__rwlock_acquire(r, 0, abstime);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t* r,
                               const struct timespec* abstime) {
  if (abstime == NULL) return EINVAL;
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  return lt__rwlock_acquire(r, 1, abstime);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* r) {
  uint32_t l;
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  l = lt_at_load32(&r->lock);
  if ((l & LT__RW_WRITER) != 0u) return EBUSY;
  if (l == LT__RW_READERS) return EAGAIN; /* reader cap (L5) */
  if (!lt_at_cas32(&r->lock, l, l + 1u)) return EBUSY;
  LT_TSAN_ACQUIRE(r);
  return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* r) {
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  if (!lt_at_cas32(&r->lock, 0u, LT__RW_WRITER)) return EBUSY;
  LT_TSAN_ACQUIRE(r);
  return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t* r) {
  uint32_t l;
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  l = lt_at_load32(&r->lock);
  if (l == 0u) return EINVAL; /* unlocking an unlocked rwlock */
  LT_TSAN_RELEASE(r);
  if ((l & LT__RW_WRITER) != 0u) {
    /* Writer release: free the word, then bump and notify both
     * sequence words (readers first in code order only; the
     * classes race from here, which reader-preference accepts). */
    lt_at_store32(&r->lock, 0u);
    lt_at_fetch_add32(&r->rseq, 1u);
    lt__notify(&r->rseq, -1);
    lt_at_fetch_add32(&r->wseq, 1u);
    lt__notify(&r->wseq, -1);
    return 0;
  }
  /* Reader release: only the last departing reader can unblock
   * a writer. */
  if (lt_at_fetch_sub32(&r->lock, 1u) == 1u) {
    lt_at_fetch_add32(&r->wseq, 1u);
    lt__notify(&r->wseq, 1);
  }
  return 0;
}

int pthread_rwlock_init(pthread_rwlock_t* r, const pthread_rwlockattr_t* attr) {
  if (r == NULL) return EINVAL;
  if (attr != NULL) {
    if (attr->pshared == PTHREAD_PROCESS_SHARED) return ENOTSUP;
    if (attr->pshared != PTHREAD_PROCESS_PRIVATE) return EINVAL;
  }
  lt_at_store32(&r->lock, 0u);
  lt_at_store32(&r->rseq, 0u);
  lt_at_store32(&r->wseq, 0u);
  lt_at_store32(&r->reserved, 0u);
  return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* r) {
  if (lt__rwlock_validate(r) != 0) return EINVAL;
  if (lt_at_load32(&r->lock) != 0u) return EBUSY;
  lt_at_store32(&r->reserved, LT__DESTROYED);
  return 0;
}

/* ---- rwlock attributes ---------------------------------------------------------- */

int pthread_rwlockattr_init(pthread_rwlockattr_t* attr) {
  if (attr == NULL) return EINVAL;
  attr->pshared = PTHREAD_PROCESS_PRIVATE;
  return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t* attr) {
  if (attr == NULL) return EINVAL;
  return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* attr, int pshared) {
  if (attr == NULL) return EINVAL;
  if (pshared != PTHREAD_PROCESS_PRIVATE &&
      pshared != PTHREAD_PROCESS_SHARED) {
    return EINVAL;
  }
  attr->pshared = pshared;
  return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t* attr,
                                  int* pshared) {
  if (attr == NULL || pshared == NULL) return EINVAL;
  *pshared = attr->pshared;
  return 0;
}

/* ---- once -------------------------------------------------------------------------
 * State 0 fresh, 1 running, 2 done. The CAS winner runs the
 * routine with no lock held (the routine may itself use any
 * LithoThread primitive), then stores 2 and notifies the word;
 * losers sleep on the word expecting 1, so the store's notify
 * (or the wait's entry value check) releases them to observe 2.
 * Review M4: if the routine exits the thread (pthread_exit, from
 * the routine or anything it calls) the once is rolled back to 0
 * and its waiters woken, so the next caller runs the routine
 * again, matching POSIX's cancellation answer ("as if
 * pthread_once was never called"). Each running once is pushed
 * on a per-thread chain whose nodes live in pthread_once's own
 * frame, still alive when pthread_exit walks the chain (before
 * its longjmp). Not covered, documented in docs/LITHOTHREAD.md:
 * a C++ exception thrown through init_routine (this C file has no
 * unwind cleanups) and a direct lw_exit bypassing pthread_exit. */
typedef struct lt_once_node {
  pthread_once_t* once;
  struct lt_once_node* prev;
} lt_once_node_t;
static LT__THREAD_LOCAL lt_once_node_t* lt__once_top;

static void lt__once_abandon_all(void) {
  lt_once_node_t* n = lt__once_top;
  lt__once_top = NULL;
  while (n != NULL) {
    lt_at_store32(&n->once->state, 0u);
    lt__notify(&n->once->state, -1);
    n = n->prev;
  }
}

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void)) {
  if (once_control == NULL || init_routine == NULL) return EINVAL;
  for (;;) {
    uint32_t s = lt_at_load32(&once_control->state);
    if (s == 2u) return 0;
    if (s == 0u && lt_at_cas32(&once_control->state, 0u, 1u)) {
      lt_once_node_t node;
      node.once = once_control;
      node.prev = lt__once_top;
      lt__once_top = &node;
      init_routine();
      lt__once_top = node.prev;
      lt_at_store32(&once_control->state, 2u);
      lt__notify(&once_control->state, -1);
      return 0;
    }
    if (s == 1u) {
      lt__wait32(&once_control->state, 1u, -1);
    }
  }
}

/* ---- thread-specific data ---------------------------------------------------------
 * A fixed table of PTHREAD_KEYS_MAX slots; each carries its
 * destructor, a used flag, and a generation that key_delete
 * bumps. Per-thread values live in the record's lazy array,
 * each stamped with the generation it was set under: a value
 * whose generation no longer matches is a leftover of a deleted
 * incarnation and reads as NULL, and the exit runner skips it.
 * Slot fields are read with atomic loads outside the key lock
 * (get / setspecific are hot and per-thread); the lock guards
 * create / delete table surgery only. */

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  pthread_key_t k;
  if (key == NULL) return EINVAL;
  lt__spin_lock(&lt__key_lock);
  for (k = 0; k < PTHREAD_KEYS_MAX; ++k) {
    if (lt__keys[k].used == 0u) {
      lt__keys[k].dtor = destructor;
      /* The destructor store must land before the used store
       * publishes the slot; both are seq-cst atomics here in
       * program order, which is the order readers observe. */
      lt_at_store32(&lt__keys[k].used, 1u);
      lt__spin_unlock(&lt__key_lock);
      *key = k;
      return 0;
    }
  }
  lt__spin_unlock(&lt__key_lock);
  return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
  if (key >= PTHREAD_KEYS_MAX) return EINVAL;
  lt__spin_lock(&lt__key_lock);
  if (lt__keys[key].used == 0u) {
    lt__spin_unlock(&lt__key_lock);
    return EINVAL;
  }
  lt_at_store32(&lt__keys[key].used, 0u);
  lt__keys[key].dtor = NULL;
  /* Retire every outstanding value of this incarnation in one
   * move: the generation moves on, and stale stamps stop
   * matching everywhere at once. */
  lt_at_store32(&lt__keys[key].gen, lt_at_load32(&lt__keys[key].gen) + 1u);
  lt__spin_unlock(&lt__key_lock);
  return 0;
}

int pthread_setspecific(pthread_key_t key, const void* value) {
  lt_rec_t* rec;
  lt_key_slot_t slot;
  if (key >= PTHREAD_KEYS_MAX) return EINVAL;
  lt__key_slot_read(key, &slot);
  if (slot.used == 0u) return EINVAL;
  rec = lt__self_rec();
  if (rec == NULL) return ENOMEM;
  if (rec->key_values == NULL) {
    rec->key_values = (lt_kv_t*)calloc(PTHREAD_KEYS_MAX, sizeof(lt_kv_t));
    if (rec->key_values == NULL) return ENOMEM;
  }
  rec->key_values[key].value = (void*)value;
  rec->key_values[key].gen = slot.gen;
  return 0;
}

void* pthread_getspecific(pthread_key_t key) {
  lt_rec_t* rec = lt__current_rec;
  lt_key_slot_t slot;
  if (key >= PTHREAD_KEYS_MAX) return NULL;
  if (rec == NULL || rec->key_values == NULL) return NULL;
  lt__key_slot_read(key, &slot);
  if (slot.used == 0u || slot.gen != rec->key_values[key].gen) return NULL;
  return rec->key_values[key].value;
}

/* ---- the wait / notify seam, wasm -------------------------------------------------
 * On wasm the seam is the instruction pair itself:
 * memory.atomic.wait32 (whose result codes ARE the seam's:
 * 0 woken, 1 value-not-equal, 2 timed out, so this is a
 * pass-through) and memory.atomic.notify (returns the number
 * of waiters woken). Both operate on the shared linear memory
 * every LithoThread word lives in. Off-wasm, the native
 * back-end file provides the seam instead; exactly one
 * definition links. */
#if defined(__wasm__)
int lt__wait32(volatile uint32_t* addr, uint32_t expected, int64_t timeout_ns) {
  return (int)__builtin_wasm_memory_atomic_wait32((int*)(void*)addr,
                                                  (int)expected, timeout_ns);
}

uint32_t lt__notify(volatile uint32_t* addr, int32_t count) {
  return (uint32_t)__builtin_wasm_memory_atomic_notify((int*)(void*)addr,
                                                       (int)count);
}
#endif
