/* lithoweave.h ... LithoWeave, the spawn / entry weave of LithoRoot.
 *
 * LithoWeave is the thin layer directly over the LithoSys thread
 * contract (see lithosys.h, lithosys_thread_spawn). Its whole job:
 *
 *   - spawn a thread from a descriptor (entry, arg, stack request,
 *     debug name) through the one frozen host import;
 *   - own the thread control block whose FIRST WORD is the tid cell
 *     the frozen exit protocol is built on;
 *   - join and detach, where join is an atomic wait on the tid cell;
 *   - run per-thread TLS destructors at thread exit, in the pass
 *     discipline documented below;
 *   - provide lw_exit, the in-module thread exit.
 *
 * It is deliberately NOT a pthread API and contains no mutexes, no
 * condition variables and no thread-local key machinery: LithoThread
 * (the next piece) builds that surface on top of this weave. The
 * layering in the end state is:
 *
 *   LithoLibC pthread -> LithoThread -> LithoWeave -> LithoSys (host)
 *
 * Status codes: every fallible function returns a lithosys_err_t
 * value (LITHOSYS_OK on success). The LithoSys numbering already
 * matches LithoLibC's errno numbering, so the layers above translate
 * mechanically.
 *
 * The spawn block layout is frozen with this header: the control
 * block passed to lithosys_thread_spawn as start_arg begins with
 * two words a host back-end may read:
 *
 *   word 0 (offset 0): the tid cell (uint32_t). Holds the thread's
 *     tid (a positive value) from before spawn until the host
 *     stores 0 into it and notifies its address when the thread's
 *     start function returns. That store is the release edge of the
 *     whole protocol: everything the thread wrote before returning
 *     (including its return value, stored in the control block) is
 *     visible to a joiner that observes the 0.
 *   word 1 (offset 4): the requested stack size in bytes
 *     (uint32_t; 0 means the host default). A back-end that can
 *     honor per-thread stacks reads it from here; the frozen spawn
 *     import has no stack parameter of its own.
 *
 * This header is strict portable C99: fixed include set, no
 * platform headers, extern "C" guarded.
 */
#ifndef LITHOWEAVE_H
#define LITHOWEAVE_H

#include <stddef.h>
#include <stdint.h>

#include "lithosys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LITHOWEAVE_VERSION 1

/* Cap on TLS destructor passes at thread exit. One pass drains the
 * destructor list as it stood when the pass began; destructors
 * registered during a pass run in the NEXT pass. Passes repeat
 * until a pass begins with an empty list, but never more than this
 * many: a destructor that endlessly registers another destructor
 * is a caller bug, and hanging the exiting thread (and everyone
 * joined on it) is a worse failure than dropping the remainder.
 * Dropped destructors are freed without being run, and the drop is
 * the documented behavior, not an accident. */
#define LW_TLS_DTOR_PASS_LIMIT 8

/* Longest thread name retained in the control block, including the
 * terminating NUL. Names are debug labels only. */
#define LW_NAME_MAX 24

/* Thread entry point, pthread-shaped: takes the descriptor's arg,
 * returns a value joiners receive. */
typedef void* (*lw_start_fn)(void* arg);

/* Spawn descriptor. entry is required; arg is passed through
 * untouched; stack_size is a request in bytes (0 = host default),
 * honored where the back-end can (see the spawn block note above:
 * on the wasm host the per-thread stack is provisioned by the
 * toolchain, not through this surface, so there the value is
 * recorded and validated, not acted on); name is a debug label,
 * copied into the control block (truncated to LW_NAME_MAX), so the
 * caller's string need not outlive the call. */
typedef struct lw_descriptor {
  lw_start_fn entry;
  void* arg;
  size_t stack_size;
  const char* name;
} lw_descriptor_t;

/* The thread control block. Opaque: its layout beyond the two
 * frozen spawn words is the weave's own business. */
typedef struct lw_control lw_control_t;

/* A thread handle: a plain value, copied freely. tid is the
 * thread's id (positive) and stays valid after the thread exits,
 * which the bare tid cell does not (the host zeroes it). Treat the
 * fields as read-only; a zeroed handle (control NULL, tid 0) is
 * the invalid handle. A handle is consumed by a successful join:
 * using it afterwards returns LITHOSYS_EINVAL. */
typedef struct lw_thread {
  lw_control_t* control;
  int32_t tid;
} lw_thread_t;

/* Spawn a thread per the descriptor. On success returns
 * LITHOSYS_OK and fills out_handle; on failure returns a
 * lithosys_err_t code (most often LITHOSYS_EAGAIN when the host
 * refuses a thread, LITHOSYS_ENOMEM when the control block cannot
 * be allocated, LITHOSYS_EINVAL for a bad descriptor) and
 * out_handle is untouched. The thread's control block comes from
 * LithoLibC malloc (WaferHeap underneath), so a default heap must
 * exist before the first spawn, exactly as for any malloc caller. */
lithosys_err_t lw_spawn(const lw_descriptor_t* desc, lw_thread_t* out_handle);

/* Join: wait (atomic wait on the tid cell, per the frozen
 * protocol) until the thread's start function has returned, then
 * collect its return value into out_retval (which may be NULL)
 * and reap the control block. Returns LITHOSYS_OK on success.
 * LITHOSYS_EINVAL when the handle is unknown or already consumed,
 * when the thread was detached, when a join was already claimed,
 * or when a thread tries to join itself. A failed join never
 * consumes the handle except where the state itself forbids any
 * join (detached / already claimed), as documented per case in
 * the implementation note. */
lithosys_err_t lw_join(lw_thread_t th, void** out_retval);

/* Detach: mark the thread detached. Its control block is reaped
 * lazily once it has finished and its cell has been zeroed by the
 * host (reclamation happens inside later weave calls; see the
 * design note). Returns LITHOSYS_OK, or LITHOSYS_EINVAL for an
 * unknown handle, an already detached thread, or a thread whose
 * join was already claimed. */
lithosys_err_t lw_detach(lw_thread_t th);

/* The tid carried by a handle (0 for an invalid handle). */
int32_t lw_thread_tid(lw_thread_t th);

/* Address of the handle's tid cell: the protocol word itself, for
 * diagnostics and tests that poll completion. NULL for an invalid
 * handle. The pointer is valid only while the control block lives:
 * until a successful join, or (for a detached thread) until the
 * block is reaped by a later weave call. */
const uint32_t* lw_thread_cell(lw_thread_t th);

/* Attach the calling thread as the process main thread (tid 1).
 * Spawned threads are attached by the weave's trampoline before
 * their entry runs; the main thread is attached explicitly, once,
 * at startup. Calling it on an already attached thread is a
 * harmless no-op returning LITHOSYS_OK. An unattached non-main
 * thread that touches the weave (lw_self, lw_current_tid,
 * lw_thread_atexit) attaches lazily as an unjoinable self thread
 * with a fresh tid; that path exists so foreign host threads can
 * register TLS destructors without ceremony. */
lithosys_err_t lw_main_thread_init(void);

/* Handle for the calling thread. An unattached caller is attached
 * lazily (see lw_main_thread_init). On allocation failure the
 * invalid handle (tid 0) comes back; lw_current_tid then reports
 * 0. */
lw_thread_t lw_self(void);

/* Tid of the calling thread, attaching lazily as lw_self does.
 * 0 only when attachment itself failed (out of memory). */
int32_t lw_current_tid(void);

/* Address of the calling thread's own tid cell, or NULL when the
 * caller could not be attached. */
const uint32_t* lw_self_cell(void);

/* Register a TLS destructor for the CALLING thread: fn(arg) runs
 * when that thread exits (through the trampoline or lw_exit), in
 * the pass discipline of LW_TLS_DTOR_PASS_LIMIT: reverse
 * registration order within a pass, registrations made during a
 * pass deferred to the next pass. This is the hook LithoThread's
 * pthread_key machinery and LithoAbi's __cxa_thread_atexit build
 * on. Returns LITHOSYS_OK, LITHOSYS_EINVAL for a NULL fn, or
 * LITHOSYS_ENOMEM when the registration node cannot be allocated. */
lithosys_err_t lw_thread_atexit(void (*fn)(void*), void* arg);

/* Run the calling thread's pending TLS destructors now, in the
 * same pass discipline as thread exit, leaving the list empty.
 * The trampoline and lw_exit call this; it is public because the
 * main thread's exit path (owned by startup code and LithoAbi)
 * needs the same service. A no-op when nothing is registered.
 *
 * Its callers are the exit paths: the trampoline, lw_exit, and
 * the main thread's exit path. Calling it bare, mid-life, is
 * legal but carries one sharp edge (audit finding C-F9,
 * accepted and documented in fix pass 2): the destructors run
 * early, and if one of them calls lw_exit, the exit request is
 * routed to this call's own landing pad... the remaining
 * destructors are abandoned and the call returns, leaving the
 * thread running with its destructor list already consumed. A
 * mid-life caller must not let the destructors it runs call
 * lw_exit. */
void lw_run_tls_destructors(void);

/* Review M1: release a lazily attached FOREIGN thread (one the
 * weave did not spawn and that is not the main thread). Runs its
 * TLS destructors, then unlinks and frees its control block. Such a
 * thread's block is otherwise never freed: no host hook tells the
 * weave that a foreign thread ended (the frozen host surface has
 * none), so a foreign thread that touched the weave (directly, or
 * through errno / pthread_self / keys) must call this (or
 * lithothread_release_self, which calls it) as its last LithoRoot
 * act. No-op on spawned threads and on the main thread. A later
 * weave call on the same thread simply attaches it afresh. */
void lw_release_self(void);

/* Exit the calling thread with a return value, as if its entry
 * had returned it: the entry is unwound (setjmp/longjmp inside the
 * trampoline), TLS destructors run, and the thread's start
 * function returns to the host, which completes the cell
 * protocol. Never returns to the caller. Called during the
 * destructor phase itself, it abandons the remaining destructors
 * and lets the exit already in progress continue. Called from a
 * thread the weave did
 * not spawn (main or foreign), it runs that thread's destructors
 * and then ends the whole instance through lithosys_exit(0):
 * on the wasm host the main thread returning IS instance exit,
 * and this keeps the meaning identical. */
#if defined(_MSC_VER)
__declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((__noreturn__))
#endif
void lw_exit(void* retval);

/* The frozen thread entry export (name fixed by the LithoSys
 * freeze; declared here, not in lithosys.h, by the freeze's own
 * ruling). The host calls this on the new worker with the
 * start_arg that lithosys_thread_spawn was given. It is the
 * weave's trampoline: attach, run the entry, run TLS destructors,
 * mark the control block done, and RETURN. Returning is the
 * in-module completion signal: the host wrapper then stores 0
 * into the tid cell, notifies its address, and retires the
 * worker. The module side never writes the cell after spawn. */
void lithosys_thread_start(uint32_t start_arg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOWEAVE_H */
