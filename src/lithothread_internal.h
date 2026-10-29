/* lithothread_internal.h ... LithoThread's wiring header (internal).
 *
 * Shared by lithothread.c, lithothread_errno.c and
 * lithothread_heap.c, and read by startup code for the two
 * bring-up calls documented at the bottom. Not a public header:
 * engine code includes <pthread.h> and nothing here.
 *
 * Contents: the registry spinlock, the per-thread record every
 * LithoThread thread carries, and the seam between the errno
 * accessor (Piece 3's indirection, given its real body here) and
 * the thread machinery.
 */
#ifndef LITHOTHREAD_INTERNAL_H
#define LITHOTHREAD_INTERNAL_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "lithoweave.h"
#include "lithothread_atomic.h"
#include "lithothread_wait.h"

/* ---- registry spinlock ----------------------------------------------
 * Test-and-test-and-set over one word, the weave lock's shape.
 * Guards the thread record registry and the dead-record list.
 * Never held across a wait, a user callback, or an allocation:
 * callers collect under it and act after releasing it. */
typedef struct {
  volatile uint32_t word;
} lt_spin_t;

#define LT_SPIN_INIT { 0u }

static inline void lt__spin_lock(lt_spin_t* s) {
  while (lt_at_exchange32(&s->word, 1u) != 0u) {
    while (lt_at_load32(&s->word) != 0u) lt_at_spin_pause();
  }
}

static inline void lt__spin_unlock(lt_spin_t* s) {
  lt_at_store32(&s->word, 0u);
}

/* ---- per-thread record ------------------------------------------------
 * One per thread that LithoThread knows: spawned by pthread_create
 * (the record is allocated by the creator, before the spawn), or
 * attached lazily when a foreign thread (the main thread first of
 * all) first touches TLS, keys, or its errno slot.
 *
 * Lifetime: a joinable record is freed by its joiner after the
 * weave join completes (lw_join returns only once the tid cell
 * reads 0, so that free is already gated on the thread having
 * fully returned). A detached record is unlinked and queued by
 * its own key-destructor runner, or by a racing pthread_detach,
 * and freed lazily by a later pthread_create / pthread_detach,
 * the same lazy-reap shape the weave uses for its own detached
 * control blocks. The free itself is gated, at reap time, on the
 * record's tid cell reading 0: the key runner finishing is NOT
 * the thread finishing, because weave destructor passes
 * registered during the key run execute after the runner, and
 * those destructors can still reach the record through
 * lt__current_rec (errno, getspecific). The cell reads 0 only
 * once the host completes the exit protocol, after the whole
 * trampoline has returned, so a record whose cell still reads
 * nonzero stays queued and the gate is retried at the next reap
 * (audit fix pass 2, C-F2). A lazily attached record
 * (spawned == 0) lives for the process: its thread has no
 * joiner by construction.
 *
 * Field discipline:
 *   handle ........ set by pthread_create before it returns; for
 *                   attached records, the weave self handle.
 *   detached ...... written under the registry lock (create from
 *                   the attribute, detach later); read by the
 *                   key runner without the lock after the owning
 *                   thread is already in its exit phase, where a
 *                   racing pthread_detach is the only writer and
 *                   either observed value leads to a correct reap
 *                   (detach reaps exited records itself).
 *   join_claimed .. under the registry lock.
 *   retval ........ written by the exiting thread before it
 *                   returns to the weave; read by the joiner only
 *                   after the weave join observes the host's
 *                   zero store into the tid cell, whose release
 *                   edge orders the write.
 *   exit_value .... the pthread_exit value: consumed by the
 *                   trampoline on the longjmp path, and by the
 *                   joiner (in place of lw_join's value) when
 *                   exit_override is set. Written by the owning
 *                   thread; the joiner reads it under the same
 *                   cell release edge as retval.
 *   exit_phase .... set by the owning thread's trampoline just
 *                   before it returns, once the weave exit phase
 *                   follows and the pthread_exit landing pad is
 *                   dead; read by pthread_exit on the owning
 *                   thread only (atomic for uniformity).
 *   exit_override . set by pthread_exit when it is called with
 *                   the exit phase already begun (from a TLS or
 *                   key destructor); tells the joiner to answer
 *                   exit_value rather than the weave's collected
 *                   value, which the trampoline froze when it
 *                   returned (audit fix pass 2, C-F3).
 *   errno_value ... the thread's errno slot itself. Touched only
 *                   through __litholibc_errno_location, only ever
 *                   by the owning thread.
 *   key_values .... lazy array of LT_KEYS_MAX entries, allocated
 *                   on the first pthread_setspecific. Touched only
 *                   by the owning thread and by its key runner,
 *                   which runs on that same thread at exit.
 */
typedef struct lt_kv {
  void* value;
  uint32_t gen; /* key generation the value belongs to */
} lt_kv_t;

typedef struct lt_rec {
  lw_thread_t handle;
  void* retval;
  void* exit_value;
  jmp_buf exit_pad; /* pthread_exit landing pad (spawned threads) */
  int spawned;
  volatile uint32_t detached;
  volatile uint32_t exit_phase_done; /* key runner finished (atomic) */
  volatile uint32_t exit_phase;      /* trampoline returned (atomic) */
  volatile uint32_t exit_override;   /* late pthread_exit (atomic) */
  volatile uint32_t retire_claimed;  /* reap ownership (atomic CAS) */
  int join_claimed;
  int errno_value;
  lt_kv_t* key_values;
  struct lt_rec* next; /* registry chain / dead list chain */
} lt_rec_t;

/* The calling thread's record pointer. Defined in
 * lithothread_errno.c (a deliberately boring translation unit:
 * see its header for why), written by lithothread.c's trampoline
 * and attach path, read by the errno accessor's fast path. */
#if defined(_MSC_VER)
#define LT__THREAD_LOCAL __declspec(thread)
#else
#define LT__THREAD_LOCAL __thread
#endif
extern LT__THREAD_LOCAL lt_rec_t* lt__current_rec;

/* The calling thread's record, or NULL when the thread is unknown
 * to LithoThread and cannot be attached (allocation failure).
 * Attaching registers the key-destructor runner with the weave,
 * so an attached thread's key values are destroyed at its exit
 * exactly like a spawned thread's. */
lt_rec_t* lt__self_rec(void);

/* Unconditional attach used by the errno accessor's slow path:
 * identical to lt__self_rec (kept as a separate name so the
 * accessor's contract reads plainly at its call site). */
lt_rec_t* lt__attach_rec(void);

/* Run the calling thread's pthread key destructors now, in the
 * POSIX iteration discipline. Registered with the weave once per
 * record (as the record's key runner); also called directly by
 * pthread_exit's foreign-thread path. */
void lt__run_key_destructors(lt_rec_t* rec);

/* ---- bring-up (called by startup, not by engine code) -----------------
 *
 * __litholibc_errno_arm_tls: Piece 3's errno accessor starts life
 * returning one process-wide slot, exactly as src/libc/errno.c
 * behaves today. Startup calls this once the default heap exists
 * and the main thread is attached (lw_main_thread_init), arming
 * the accessor's per-thread slow path. Before the call the
 * accessor never reaches into LithoThread at all, which keeps
 * early-boot and loader-time errno writes on the plain slot.
 * Declared in lithothread_errno.c. */
void __litholibc_errno_arm_tls(void);

/* lithothread_heap_options: the WaferHeap glue. Returns a
 * wh_options_t with the lock hooks wired to a LithoThread mutex,
 * grow fields zeroed for the caller to fill. Declared here
 * because its type comes from waferheap.h; defined in
 * lithothread_heap.c. */
#include "waferheap.h"
wh_options_t lithothread_heap_options(void);

#endif /* LITHOTHREAD_INTERNAL_H */
