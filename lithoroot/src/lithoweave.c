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
 * No locks: the registry is a fixed array of LW_MAX_THREADS control
 * blocks. A slot is claimed, flagged and released only by
 * compare-and-swap on its state word (see "The registry" below);
 * exactly one party (joiner, reaper, or the thread itself) wins the
 * release of each incarnation. Blocks are never freed, so a scan
 * racing a release reads a recycled slot at worst, and the
 * generation in the word makes any CAS against it fail.
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

/* The registry is a fixed slot array with no lock (JD's doctrine).
 * Each slot's state, flags and a generation count share one 32-bit
 * word that only ever changes by compare-and-swap:
 *   bits 0..7   flags (LW__F_*)
 *   bits 8..9   slot state: FREE, CLAIMING (being filled), LIVE
 *   bits 10..31 generation, bumped on every claim
 * A CAS against a word read earlier fails if the slot was released
 * and re-claimed in between (the generation moved), which is what
 * makes a stale view of a recycled slot harmless. Control blocks
 * live in the array and are never freed, so no reader can touch
 * freed memory. */
#define LW__F_DETACHED 0x01u     /* no join will ever be accepted */
#define LW__F_JOIN_CLAIMED 0x02u /* a join owns the reap */
#define LW__F_SELF 0x04u         /* born by lazy attach, not by spawn */
#define LW__F_MAIN 0x08u         /* holds tid 1 */
#define LW__F_DONE 0x10u         /* trampoline finished */
#define LW__F_REAPED 0x20u       /* one reaper won the release */
#define LW__F_MASK 0xFFu
#define LW__S_MASK 0x300u
#define LW__S_FREE 0x000u
#define LW__S_CLAIMING 0x100u
#define LW__S_LIVE 0x200u
#define LW__GEN_ONE 0x400u
#define LW__KEEP_GEN (~(uint32_t)0x3FFu)

struct lw_control {
  volatile uint32_t tid_cell; /* offset 0: frozen spawn word 0 */
  uint32_t stack_word;        /* offset 4: frozen spawn word 1 */
  int32_t tid;                /* the tid, kept after the cell zeroes */
  lw_start_fn entry;
  void* entry_arg;
  void* retval;
  int spawned;    /* born by lw_spawn (trampoline armed its jumps) */
  int exiting;    /* destructor phase running on this thread */
  lw_dtor_node_t* dtors; /* only the owning thread touches this list */
  lw_dtor_node_t* dtors_inflight; /* the pass currently running,
                                   * anchored here so an lw_exit
                                   * longjmp out of a destructor
                                   * leaves the remainder reachable
                                   * for the landing pad to drop */
  jmp_buf jmp_entry;     /* trampoline's landing pad for lw_exit */
  jmp_buf jmp_dtor;      /* destructor phase landing pad */
  char name[LW_NAME_MAX];
  volatile uint32_t tid_word; /* tid, atomic: registry scans read it */
  volatile uint32_t word;     /* flags | slot state | generation */
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
static lw_control_t lw__slots[LW_MAX_THREADS]; /* the registry */
static volatile uint32_t lw__next_tid = 2u;    /* tid allocator */
static volatile uint32_t lw__main_claimed;     /* tid 1 taken */
static LW__THREAD_LOCAL lw_control_t* lw__current;

/* The heap gives a thread's arena back when the thread ends
 * (lithothread_heap.c). Weak so LithoWeave links without it. */
extern void lh_thread_release(void) __attribute__((weak));

static uint32_t lw__word(lw_control_t* c) { return lw_at_load32(&c->word); }

/* Review L7: tids are positive int32. The allocator wraps from
 * INT32_MAX back to 2 instead of running negative, and skips a tid
 * still held by a claimed slot, so a live thread's tid is never
 * reissued. fetch_add makes every draw unique until the 2^31 wrap. */
static int32_t lw__alloc_tid(void) {
  for (;;) {
    uint32_t t = lw_at_fetch_add32(&lw__next_tid, 1u);
    unsigned i;
    int live = 0;
    if (t < 2u || t > 0x7FFFFFFFu) {
      uint32_t seen = t + 1u;
      (void)lw_at_cas32(&lw__next_tid, &seen, 2u);
      continue;
    }
    for (i = 0; i < LW_MAX_THREADS; ++i) {
      if ((lw__word(&lw__slots[i]) & LW__S_MASK) != LW__S_FREE &&
          lw_at_load32(&lw__slots[i].tid_word) == t) {
        live = 1;
        break;
      }
    }
    if (!live) return (int32_t)t;
  }
}

/* Claim a free slot: CAS FREE -> CLAIMING (bumping the generation),
 * fill it, then publish LIVE with the given flags. tid 0 means draw
 * one. The cell carries the tid from here until the host zeroes it
 * at exit. Returns NULL when every slot is taken. */
static lw_control_t* lw__claim(uint32_t flags, int32_t tid) {
  unsigned i;
  for (i = 0; i < LW_MAX_THREADS; ++i) {
    lw_control_t* c = &lw__slots[i];
    uint32_t w = lw__word(c);
    uint32_t mine;
    if ((w & LW__S_MASK) != LW__S_FREE) continue;
    mine = ((w & LW__KEEP_GEN) + LW__GEN_ONE) | LW__S_CLAIMING;
    if (!lw_at_cas32(&c->word, &w, mine)) continue;
    memset((char*)c + offsetof(lw_control_t, stack_word), 0,
           offsetof(lw_control_t, tid_word) - offsetof(lw_control_t, stack_word));
    if (tid == 0) tid = lw__alloc_tid();
    c->tid = tid;
    lw_at_store32(&c->tid_word, (uint32_t)tid);
    lw_at_store32(&c->tid_cell, (uint32_t)tid);
    lw_at_store32(&c->word, (mine & LW__KEEP_GEN) | LW__S_LIVE | flags);
    return c;
  }
  return NULL;
}

/* Hand a slot back. Only the party that owns the reap calls this
 * (the joiner, the winning reaper, or the thread itself). */
static void lw__release(lw_control_t* c) {
  uint32_t w = lw__word(c);
  lw_at_store32(&c->tid_word, 0u);
  lw_at_store32(&c->word, w & LW__KEEP_GEN);
}

/* Set flag bits on a slot's word by CAS. Returns the new word. */
static uint32_t lw__set_flags(lw_control_t* c, uint32_t bits) {
  uint32_t w = lw__word(c);
  while (!lw_at_cas32(&c->word, &w, w | bits)) {
  }
  return w | bits;
}

/* Resolve a handle to its slot by identity: the pointer must be a
 * slot of the array, the slot LIVE, and its tid the handle's tid.
 * Returns the word observed, which the caller CASes against: the
 * CAS fails if the slot was recycled after this check. */
static int lw__find(int32_t tid, const lw_control_t* ptr, uint32_t* out_word) {
  uintptr_t base = (uintptr_t)&lw__slots[0];
  uintptr_t p = (uintptr_t)ptr;
  uint32_t w;
  if (p < base || p >= base + sizeof(lw__slots) ||
      (p - base) % sizeof(lw_control_t) != 0u) {
    return 0;
  }
  w = lw__word((lw_control_t*)ptr);
  if ((w & LW__S_MASK) != LW__S_LIVE) return 0;
  if (lw_at_load32(&((lw_control_t*)ptr)->tid_word) != (uint32_t)tid) return 0;
  *out_word = w;
  return 1;
}

/* Release a detached, DONE block whose cell the host zeroed. The CAS
 * that sets REAPED picks exactly one releaser per incarnation. */
static void lw__try_reap(lw_control_t* c, uint32_t w) {
  if ((w & LW__S_MASK) != LW__S_LIVE ||
      (w & (LW__F_DETACHED | LW__F_DONE | LW__F_REAPED)) !=
          (LW__F_DETACHED | LW__F_DONE)) {
    return;
  }
  if (lw_at_load32(&c->tid_cell) != 0u) return;
  if (lw_at_cas32(&c->word, &w, w | LW__F_REAPED)) lw__release(c);
}

/* Reap detached blocks that are DONE with a zeroed cell. Called at
 * the top of the public entry points that can observe completion;
 * reclamation of a detached block is lazy by design. */
static void lw__sweep(void) {
  unsigned i;
  for (i = 0; i < LW_MAX_THREADS; ++i) {
    lw__try_reap(&lw__slots[i], lw__word(&lw__slots[i]));
  }
}

/* Attach the calling thread with a self-born block (main thread and
 * foreign threads). The first attacher claims tid 1. Returns NULL
 * when the registry is full. The cell of a self-born thread holds
 * its tid for life: no host wrapper exists to zero it. */
static lw_control_t* lw__attach_self(void) {
  uint32_t zero = 0u;
  lw_control_t* ctl;
  if (lw_at_cas32(&lw__main_claimed, &zero, 1u)) {
    ctl = lw__claim(LW__F_DETACHED | LW__F_SELF | LW__F_MAIN, 1);
  } else {
    ctl = lw__claim(LW__F_DETACHED | LW__F_SELF, 0);
  }
  if (ctl == NULL) return NULL;
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
#if SIZE_MAX > 0xFFFFFFFFu
  /* Review L7: the frozen spawn word is 32 bits; refuse a request
   * it cannot carry instead of truncating it. */
  if (desc->stack_size > (size_t)0xFFFFFFFFu) return LITHOSYS_EINVAL;
#endif
  lw__sweep();

  /* A full registry is "try again later", like pthread_create. */
  ctl = lw__claim(0u, 0);
  if (ctl == NULL) return LITHOSYS_EAGAIN;
  ctl->entry = desc->entry;
  ctl->entry_arg = desc->arg;
  ctl->spawned = 1;
  ctl->stack_word = (uint32_t)desc->stack_size;
  if (desc->name != NULL) {
    size_t i = 0;
    while (i + 1 < sizeof(ctl->name) && desc->name[i] != '\0') {
      ctl->name[i] = desc->name[i];
      ++i;
    }
    ctl->name[i] = '\0';
  }

  rc = lithosys_thread_spawn((uint32_t)(uintptr_t)ctl);
  if (rc < 0) {
    /* Host refused: retire the block ourselves. No thread exists,
     * so no host store is coming; the registry removal and free
     * are safe and immediate. */
    lw__release(ctl);
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

  ctl = th.control;
  for (;;) {
    uint32_t w;
    if (!lw__find(th.tid, ctl, &w) || ctl == lw__current ||
        (w & (LW__F_DETACHED | LW__F_JOIN_CLAIMED)) != 0u) {
      return LITHOSYS_EINVAL;
    }
    if (lw_at_cas32(&ctl->word, &w, w | LW__F_JOIN_CLAIMED)) break;
  }

  /* The frozen wait: sleep on the cell while it still holds the
   * tid. The host's seq-cst store of 0 is the release edge, so
   * everything the thread wrote before returning (its return
   * value included) is visible once the 0 is observed. */
  for (;;) {
    uint32_t v = lw_at_load32(&ctl->tid_cell);
    if (v == 0u) break;
    lw__cell_wait(&ctl->tid_cell, v);
  }

  retval = ctl->retval;
  lw__release(ctl);
  if (out_retval != NULL) *out_retval = retval;
  return LITHOSYS_OK;
}

lithosys_err_t lw_detach(lw_thread_t th) {
  lw_control_t* ctl;

  if (th.control == NULL || th.tid <= 0) return LITHOSYS_EINVAL;
  lw__sweep();

  ctl = th.control;
  for (;;) {
    uint32_t w;
    if (!lw__find(th.tid, ctl, &w) ||
        (w & (LW__F_DETACHED | LW__F_JOIN_CLAIMED)) != 0u) {
      return LITHOSYS_EINVAL;
    }
    if (lw_at_cas32(&ctl->word, &w, w | LW__F_DETACHED)) {
      lw__try_reap(ctl, w | LW__F_DETACHED); /* already finished? */
      return LITHOSYS_OK;
    }
  }
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
  if (ctl == NULL || ctl->spawned || (lw__word(ctl) & LW__F_SELF) == 0u ||
      (lw__word(ctl) & LW__F_MAIN) != 0u || ctl->exiting) {
    return;
  }
  lw_run_tls_destructors();
  lw__drop_dtors(ctl); /* anything a destructor registered late */
  lw__current = NULL;
  if (lh_thread_release) lh_thread_release();
  lw__release(ctl);
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
/* Plan §3.4 rule 1: memory.atomic.wait32 traps on the browser page thread
 * and on unshared memory. Only threads this trampoline starts are host
 * workers, so only they may block; the page thread and single-thread mode
 * (no workers) never set the flag. LithoRoot's <atomic> and the static-local
 * guard read it through __lk_can_block_here and spin instead when it is 0. */
static _Thread_local int lw__can_block;
int __lk_can_block_here(void) { return lw__can_block; }

void lithosys_thread_start(uint32_t start_arg) {
  lw_control_t* ctl = (lw_control_t*)(uintptr_t)start_arg;
  lw__current = ctl;
  lw__can_block = 1;

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
  if (lh_thread_release) lh_thread_release();
  (void)lw__set_flags(ctl, LW__F_DONE);

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
