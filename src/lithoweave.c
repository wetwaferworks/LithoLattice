/* lithoweave.c ... LithoWeave, the spawn / entry weave.
 *
 * Implements include/lithoweave.h over the one frozen host import
 * lithosys_thread_spawn. See docs/LITHOWEAVE.md for the design
 * note, including the cell protocol as implemented on both sides
 * and the answer to the freeze's open question 4.
 *
 * Structure in one paragraph: lw_spawn allocates a control block
 * (LithoLibC malloc, so WaferHeap underneath), writes the tid into
 * the block's first word (the tid cell) and the stack request into
 * the second word, registers the block, and calls the host import
 * with the block's address. The host starts a worker whose entry
 * is this file's lithosys_thread_start (the trampoline): it
 * attaches the block as the thread's own, runs the entry under a
 * setjmp arm (lw_exit lands there), runs the TLS destructor
 * passes, marks the block DONE, and returns. Returning is the
 * completion signal: the host wrapper then stores 0 into the tid
 * cell and notifies its address. lw_join waits on the cell,
 * collects the return value, and reaps the block.
 *
 * Locking: one spinlock guards the registry and every control
 * block state transition. It is never held across a wait, across
 * user code (entries, destructors), or across a malloc / free:
 * reaping collects blocks under the lock and frees them after
 * unlocking, in that order, everywhere.
 *
 * Dependencies: <stdlib.h> and <string.h> resolve to LithoLibC in
 * the LithoRoot build (control blocks and destructor nodes are
 * ordinary malloc traffic). <setjmp.h> is NOT yet a LithoRoot
 * header: the weave uses setjmp / longjmp for lw_exit's unwind,
 * and providing setjmp in-module is LithoLibC stdlib business that
 * the toolchain piece must reckon with (wasm setjmp needs the
 * toolchain's stack machinery). Flagged in the design note.
 */

#include "lithoweave.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lithoweave_atomic.h"
#include "lithoweave_internal.h"

/* ---- thread-local current pointer ----------------------------------
 * The weave's entire TLS footprint is this one pointer. On wasm it
 * relies on the toolchain's TLS support, the same facility the
 * freeze assigns to in-module setup under LithoWeave / LithoThread. */
#if defined(_MSC_VER)
#define LW__THREAD_LOCAL __declspec(thread)
#else
#define LW__THREAD_LOCAL __thread
#endif

/* ---- control block ---------------------------------------------------
 * The first two words are the frozen spawn block (lithoweave.h):
 * word 0 the tid cell, word 1 the stack request. Everything after
 * is module-private. */
typedef struct lw_dtor_node {
  void (*fn)(void*);
  void* arg;
  struct lw_dtor_node* next;
} lw_dtor_node_t;

#define LW__STATE_RUNNING 0u
#define LW__STATE_DONE 1u

#define LW__F_DETACHED 0x1u    /* no join will ever be accepted */
#define LW__F_JOIN_CLAIMED 0x2u /* a join owns the reap */
#define LW__F_SELF 0x4u        /* born by lazy attach, not by spawn */
#define LW__F_MAIN 0x8u        /* holds tid 1 */

struct lw_control {
  volatile uint32_t tid_cell; /* offset 0: frozen spawn word 0 */
  uint32_t stack_word;        /* offset 4: frozen spawn word 1 */
  int32_t tid;                /* the tid, kept after the cell zeroes */
  lw_start_fn entry;
  void* entry_arg;
  void* retval;
  uint32_t state; /* LW__STATE_*, under the registry lock */
  uint32_t flags; /* LW__F_*, under the registry lock */
  int spawned;    /* born by lw_spawn (trampoline armed its jumps) */
  int exiting;    /* destructor phase running on this thread */
  lw_dtor_node_t* dtors; /* only the owning thread touches this list */
  lw_dtor_node_t* dtors_inflight; /* the pass currently running,
                                   * anchored here so an lw_exit
                                   * longjmp out of a destructor
                                   * leaves the remainder reachable
                                   * for the landing pad to drop */
  lw_control_t* next;    /* registry chain, under the registry lock */
  jmp_buf jmp_entry;     /* trampoline's landing pad for lw_exit */
  jmp_buf jmp_dtor;      /* destructor phase landing pad */
  char name[LW_NAME_MAX];
};

/* Compile-time check of the frozen spawn block layout. */
typedef char lw__assert_cell_offset[(offsetof(lw_control_t, tid_cell) ==
                                     LW_SPAWN_CELL_OFFSET)
                                        ? 1
                                        : -1];
typedef char lw__assert_stack_offset[(offsetof(lw_control_t, stack_word) ==
                                      LW_SPAWN_STACK_OFFSET)
                                         ? 1
                                         : -1];

/* ---- globals --------------------------------------------------------- */
static volatile uint32_t lw__lock_word; /* the registry spinlock */
static lw_control_t* lw__registry;      /* live control blocks */
static volatile uint32_t lw__next_tid = 2u; /* tid allocator */
static int lw__main_claimed;              /* tid 1 taken (under lock) */
static LW__THREAD_LOCAL lw_control_t* lw__current;

static void lw__lock(void) {
  while (lw_at_exchange32(&lw__lock_word, 1u) != 0u) {
    while (lw_at_load32(&lw__lock_word) != 0u) lw_at_spin_pause();
  }
}

static void lw__unlock(void) { lw_at_store32(&lw__lock_word, 0u); }

/* Find a registered block by handle identity (tid AND pointer, so
 * a stale handle whose block was reaped and whose address was
 * recycled cannot alias a new thread). Caller holds the lock. The
 * handle's pointer itself is only ever compared, never
 * dereferenced, before this lookup succeeds. */
static lw_control_t* lw__find_locked(int32_t tid, const lw_control_t* ptr) {
  lw_control_t* c;
  for (c = lw__registry; c != NULL; c = c->next) {
    if (c == ptr && c->tid == tid) return c;
  }
  return NULL;
}

/* Unlink a block from the registry. Caller holds the lock and
 * frees the block after unlocking. */
static void lw__unlink_locked(lw_control_t* ctl) {
  lw_control_t** pp = &lw__registry;
  while (*pp != NULL && *pp != ctl) pp = &(*pp)->next;
  if (*pp == ctl) *pp = ctl->next;
}

/* Reap detached blocks that are DONE with a zeroed cell. Collected
 * under the lock, freed after it. Called at the top of the public
 * entry points that can observe completion; reclamation of a
 * detached block is therefore lazy by design (see the header). */
static void lw__sweep(void) {
  lw_control_t* dead = NULL;
  lw_control_t** pp;
  lw__lock();
  pp = &lw__registry;
  while (*pp != NULL) {
    lw_control_t* c = *pp;
    if (c->state == LW__STATE_DONE && (c->flags & LW__F_DETACHED) != 0u &&
        lw_at_load32(&c->tid_cell) == 0u) {
      *pp = c->next;
      c->next = dead;
      dead = c;
    } else {
      pp = &c->next;
    }
  }
  lw__unlock();
  while (dead != NULL) {
    lw_control_t* next = dead->next;
    free(dead);
    dead = next;
  }
}

/* Attach the calling thread with a self-born control block (main
 * thread and foreign threads). The first attacher in the process
 * claims tid 1. Returns the block, or NULL out of memory. */
static lw_control_t* lw__attach_self(void) {
  lw_control_t* ctl = (lw_control_t*)malloc(sizeof(lw_control_t));
  if (ctl == NULL) return NULL;
  memset(ctl, 0, sizeof(lw_control_t));
  ctl->flags = LW__F_DETACHED | LW__F_SELF;
  ctl->state = LW__STATE_RUNNING;
  lw__lock();
  if (!lw__main_claimed) {
    lw__main_claimed = 1;
    ctl->tid = 1;
    ctl->flags |= LW__F_MAIN;
  } else {
    ctl->tid = (int32_t)lw_at_fetch_add32(&lw__next_tid, 1u);
  }
  ctl->next = lw__registry;
  lw__registry = ctl;
  lw__unlock();
  /* The cell of a self-born thread holds its tid for life: no host
   * wrapper exists to zero it, and nothing may join this thread. */
  lw_at_store32(&ctl->tid_cell, (uint32_t)ctl->tid);
  lw__current = ctl;
  return ctl;
}

static lw_control_t* lw__self_ctl(void) {
  if (lw__current == NULL) return lw__attach_self();
  return lw__current;
}

/* ---- TLS destructors ---------------------------------------------------
 * One pass: detach the whole current list and run it head first
 * (the head is the most recently registered, so a pass runs in
 * reverse registration order). Registrations made during the pass
 * land on the fresh list and run in the next pass. Passes repeat
 * until a pass begins empty, capped at LW_TLS_DTOR_PASS_LIMIT;
 * whatever is still listed after the cap is freed without being
 * run (lw__drop_dtors), because a self-renewing destructor must
 * not hang an exiting thread and its joiners. Only the owning
 * thread ever touches its list, so no locking is needed here. */
static void lw__drop_dtors(lw_control_t* ctl) {
  lw_dtor_node_t* n = ctl->dtors;
  ctl->dtors = NULL;
  while (n != NULL) {
    lw_dtor_node_t* next = n->next;
    free(n);
    n = next;
  }
}

static void lw__run_dtors(lw_control_t* ctl) {
  unsigned pass;
  for (pass = 0; pass < LW_TLS_DTOR_PASS_LIMIT; ++pass) {
    if (ctl->dtors == NULL) return;
    ctl->dtors_inflight = ctl->dtors;
    ctl->dtors = NULL;
    while (ctl->dtors_inflight != NULL) {
      lw_dtor_node_t* node = ctl->dtors_inflight;
      void (*fn)(void*) = node->fn;
      void* arg = node->arg;
      ctl->dtors_inflight = node->next;
      free(node);
      fn(arg);
    }
  }
  if (ctl->dtors != NULL) lw__drop_dtors(ctl);
}

/* Drop everything an interrupted destructor phase left behind:
 * the un-run remainder of the in-flight pass plus any fresh
 * registrations. Called at the jmp_dtor landing pads after an
 * lw_exit longjmp out of a destructor. */
static void lw__abandon_dtors(lw_control_t* ctl) {
  lw_dtor_node_t* n = ctl->dtors_inflight;
  ctl->dtors_inflight = NULL;
  while (n != NULL) {
    lw_dtor_node_t* next = n->next;
    free(n);
    n = next;
  }
  lw__drop_dtors(ctl);
}

/* ---- spawn ----------------------------------------------------------- */
lithosys_err_t lw_spawn(const lw_descriptor_t* desc, lw_thread_t* out_handle) {
  lw_control_t* ctl;
  int32_t rc;

  if (desc == NULL || desc->entry == NULL || out_handle == NULL) {
    return LITHOSYS_EINVAL;
  }
  lw__sweep();

  ctl = (lw_control_t*)malloc(sizeof(lw_control_t));
  if (ctl == NULL) return LITHOSYS_ENOMEM;
  memset(ctl, 0, sizeof(lw_control_t));
  ctl->entry = desc->entry;
  ctl->entry_arg = desc->arg;
  ctl->spawned = 1;
  ctl->state = LW__STATE_RUNNING;
  ctl->stack_word = (uint32_t)desc->stack_size;
  if (desc->name != NULL) {
    size_t i = 0;
    while (i + 1 < sizeof(ctl->name) && desc->name[i] != '\0') {
      ctl->name[i] = desc->name[i];
      ++i;
    }
    ctl->name[i] = '\0';
  }

  ctl->tid = (int32_t)lw_at_fetch_add32(&lw__next_tid, 1u);
  /* The cell carries the tid from before the spawn call until the
   * host zeroes it at exit. This store is the last write the
   * module ever makes to the cell. */
  lw_at_store32(&ctl->tid_cell, (uint32_t)ctl->tid);

  lw__lock();
  ctl->next = lw__registry;
  lw__registry = ctl;
  lw__unlock();

  rc = lithosys_thread_spawn((uint32_t)(uintptr_t)ctl);
  if (rc < 0) {
    /* Host refused: retire the block ourselves. No thread exists,
     * so no host store is coming; the registry removal and free
     * are safe and immediate. */
    lw__lock();
    lw__unlink_locked(ctl);
    lw__unlock();
    free(ctl);
    return (lithosys_err_t)(-rc);
  }
  /* A conforming host returns the tid it read from the cell. The
   * cell is the protocol either way, so any positive return is
   * success and the handle carries the cell's tid. */
  out_handle->control = ctl;
  out_handle->tid = ctl->tid;
  return LITHOSYS_OK;
}

/* ---- join / detach ----------------------------------------------------- */
lithosys_err_t lw_join(lw_thread_t th, void** out_retval) {
  lw_control_t* ctl;
  void* retval;

  if (th.control == NULL || th.tid <= 0) return LITHOSYS_EINVAL;
  lw__sweep();

  lw__lock();
  ctl = lw__find_locked(th.tid, th.control);
  if (ctl == NULL || ctl == lw__current ||
      (ctl->flags & (LW__F_DETACHED | LW__F_JOIN_CLAIMED)) != 0u) {
    lw__unlock();
    return LITHOSYS_EINVAL;
  }
  ctl->flags |= LW__F_JOIN_CLAIMED;
  lw__unlock();

  /* The frozen wait: sleep on the cell while it still holds the
   * tid. The host's seq-cst store of 0 is the release edge, so
   * everything the thread wrote before returning (its return
   * value included) is visible once the 0 is observed. */
  for (;;) {
    uint32_t v = lw_at_load32(&ctl->tid_cell);
    if (v == 0u) break;
    lw__cell_wait(&ctl->tid_cell, v);
  }

  lw__lock();
  retval = ctl->retval;
  lw__unlink_locked(ctl);
  lw__unlock();
  free(ctl);
  if (out_retval != NULL) *out_retval = retval;
  return LITHOSYS_OK;
}

lithosys_err_t lw_detach(lw_thread_t th) {
  lw_control_t* ctl;
  int reap_now = 0;

  if (th.control == NULL || th.tid <= 0) return LITHOSYS_EINVAL;
  lw__sweep();

  lw__lock();
  ctl = lw__find_locked(th.tid, th.control);
  if (ctl == NULL ||
      (ctl->flags & (LW__F_DETACHED | LW__F_JOIN_CLAIMED)) != 0u) {
    lw__unlock();
    return LITHOSYS_EINVAL;
  }
  ctl->flags |= LW__F_DETACHED;
  if (ctl->state == LW__STATE_DONE && lw_at_load32(&ctl->tid_cell) == 0u) {
    lw__unlink_locked(ctl);
    reap_now = 1;
  }
  lw__unlock();
  if (reap_now) free(ctl);
  return LITHOSYS_OK;
}

int32_t lw_thread_tid(lw_thread_t th) { return th.control != NULL ? th.tid : 0; }

const uint32_t* lw_thread_cell(lw_thread_t th) {
  if (th.control == NULL) return NULL;
  return (const uint32_t*)&th.control->tid_cell;
}

/* ---- self ---------------------------------------------------------------- */
lithosys_err_t lw_main_thread_init(void) {
  if (lw__current != NULL) return LITHOSYS_OK;
  return lw__attach_self() != NULL ? LITHOSYS_OK : LITHOSYS_ENOMEM;
}

lw_thread_t lw_self(void) {
  lw_thread_t th;
  lw_control_t* ctl = lw__self_ctl();
  th.control = ctl;
  th.tid = ctl != NULL ? ctl->tid : 0;
  return th;
}

int32_t lw_current_tid(void) {
  lw_control_t* ctl = lw__self_ctl();
  return ctl != NULL ? ctl->tid : 0;
}

const uint32_t* lw_self_cell(void) {
  lw_control_t* ctl = lw__self_ctl();
  return ctl != NULL ? (const uint32_t*)&ctl->tid_cell : NULL;
}

/* ---- TLS destructor registration ---------------------------------------- */
lithosys_err_t lw_thread_atexit(void (*fn)(void*), void* arg) {
  lw_control_t* ctl;
  lw_dtor_node_t* node;
  if (fn == NULL) return LITHOSYS_EINVAL;
  ctl = lw__self_ctl();
  if (ctl == NULL) return LITHOSYS_ENOMEM;
  node = (lw_dtor_node_t*)malloc(sizeof(lw_dtor_node_t));
  if (node == NULL) return LITHOSYS_ENOMEM;
  node->fn = fn;
  node->arg = arg;
  node->next = ctl->dtors;
  ctl->dtors = node;
  return LITHOSYS_OK;
}

void lw_run_tls_destructors(void) {
  lw_control_t* ctl = lw__current;
  if (ctl == NULL || ctl->exiting || ctl->dtors == NULL) return;
  ctl->exiting = 1;
  if (setjmp(ctl->jmp_dtor) == 0) {
    lw__run_dtors(ctl);
  } else {
    /* lw_exit from inside a destructor abandons the rest. */
    lw__abandon_dtors(ctl);
  }
  ctl->exiting = 0;
}

void lw_release_self(void) {
  lw_control_t* ctl = lw__current;
  if (ctl == NULL || ctl->spawned || (ctl->flags & LW__F_SELF) == 0u ||
      (ctl->flags & LW__F_MAIN) != 0u || ctl->exiting) {
    return;
  }
  lw_run_tls_destructors();
  lw__drop_dtors(ctl); /* anything a destructor registered late */
  lw__lock();
  lw__unlink_locked(ctl);
  lw__unlock();
  lw__current = NULL;
  free(ctl);
}

/* ---- exit ------------------------------------------------------------------ */
void lw_exit(void* retval) {
  lw_control_t* ctl = lw__current;
  if (ctl == NULL) ctl = lw__attach_self();
  if (ctl != NULL) {
    if (ctl->exiting) {
      /* A destructor called lw_exit: abandon the rest of the
       * list and land in whichever frame is running the phase. */
      longjmp(ctl->jmp_dtor, 2);
    }
    if (ctl->spawned) {
      ctl->retval = retval;
      longjmp(ctl->jmp_entry, 1);
    }
    /* Main or foreign thread: run its destructors, then fall
     * through to ending the instance (see the header: on the
     * wasm host the main thread returning IS instance exit). */
    ctl->exiting = 1;
    if (setjmp(ctl->jmp_dtor) == 0) {
      lw__run_dtors(ctl);
    } else {
      lw__abandon_dtors(ctl);
    }
    ctl->exiting = 0;
  }
  lithosys_exit(0);
  for (;;) {
    /* lithosys_exit never returns by contract; this loop only
     * satisfies compilers that cannot see through the host. */
  }
}

/* ---- the trampoline ---------------------------------------------------------
 * The frozen start export. Sequence: attach, run the entry under
 * the entry landing pad, run the destructor passes under the
 * destructor landing pad, mark DONE, return. The host wrapper
 * completes the protocol from here: store 0 into the tid cell,
 * notify its address, retire the worker. */
void lithosys_thread_start(uint32_t start_arg) {
  lw_control_t* ctl = (lw_control_t*)(uintptr_t)start_arg;
  lw__current = ctl;

  if (setjmp(ctl->jmp_entry) == 0) {
    ctl->retval = ctl->entry(ctl->entry_arg);
  }
  /* Reached on a plain return, or by lw_exit's longjmp with the
   * return value already stored. */

  ctl->exiting = 1;
  if (setjmp(ctl->jmp_dtor) == 0) {
    lw__run_dtors(ctl);
  } else {
    lw__abandon_dtors(ctl);
  }
  ctl->exiting = 0;

  /* DONE is marked before returning: the host's store of 0 into
   * the cell follows, so any joiner that observes the 0 also
   * observes DONE and the return value. A detached block is NOT
   * freed here: its cell has not been zeroed yet, and the cell
   * must stay valid for the host's store. Reapers collect it
   * later (lw__sweep). */
  lw__lock();
  ctl->state = LW__STATE_DONE;
  lw__unlock();

  lw__current = NULL;
}

/* ---- cell wait, wasm ---------------------------------------------------
 * On wasm the wait is an instruction, not an import: wait32 on
 * the cell's address with an infinite timeout. The host's
 * Atomics.notify on the same address (JS side) is what wakes it,
 * which is exactly the wasi-threads shape the freeze froze. The
 * return value (woken / value-changed / timed out) is ignored:
 * lw_join re-loads and loops, which is correct under all three. */
#if defined(__wasm__)
void lw__cell_wait(const volatile uint32_t* cell, uint32_t expected) {
  (void)__builtin_wasm_memory_atomic_wait32((int*)(void*)cell,
                                            (int)expected, -1LL);
}
#endif
