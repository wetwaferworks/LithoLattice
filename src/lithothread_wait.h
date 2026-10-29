/* lithothread_wait.h ... the wait / notify seam (internal).
 *
 * Every LithoThread primitive sleeps on a 32-bit word in shared
 * memory and wakes sleepers on the same word. The operations are
 * futex-shaped: the wait is value-checked (the caller passes the
 * value it just loaded; the wait returns immediately when the
 * word no longer holds it), so a notify that races the entry to
 * sleep can never be lost... the sleeper either observes the new
 * value or is already blocked where the notify reaches it. That
 * one property carries the correctness of mutex, condvar, rwlock
 * and once in this piece; each primitive's note in
 * docs/LITHOTHREAD.md reduces its proof to it.
 *
 * Who provides the seam:
 *   - wasm (lithothread.c, under __wasm__): the instructions
 *     themselves, memory.atomic.wait32 / memory.atomic.notify,
 *     via the compiler builtins. These are instructions, not
 *     imports, per the LithoSys freeze.
 *   - native hosts (lithothread_native.c): a mutex + condvar
 *     registry keyed by the word's address, test scaffolding in
 *     exactly the shape of LithoWeave's native cell registry,
 *     because portable native C has no futex. A future native
 *     LithoSys back-end (Linux futex, Windows WaitOnAddress)
 *     replaces that file's role whole.
 *
 * This header is deliberately free of every other LithoRoot
 * header so the native back-end can include it while seeing
 * host headers only.
 */
#ifndef LITHOTHREAD_WAIT_H
#define LITHOTHREAD_WAIT_H

#include <stdint.h>

/* Result codes for lt__wait32 (the wasm wait32 result numbering,
 * so the wasm definition is a pass-through). */
#define LT_WAIT_WOKEN     0 /* a notify reached us (or a spurious return) */
#define LT_WAIT_NOT_EQUAL 1 /* *addr != expected when checked */
#define LT_WAIT_TIMED_OUT 2 /* the timeout expired with the value equal */

/* Block while *addr == expected, at most timeout_ns nanoseconds
 * (a negative timeout waits indefinitely). Returns one of the
 * LT_WAIT_* codes. Callers always re-load and loop, so waking
 * for any reason at all is safe; the NOT_EQUAL fast path is what
 * closes the missed-wakeup window, and TIMED_OUT is only a
 * statement about the last check: the word still held expected
 * when the deadline passed. */
int lt__wait32(volatile uint32_t* addr, uint32_t expected, int64_t timeout_ns);

/* Wake sleepers on addr: count > 0 wakes up to count sleepers,
 * count <= 0 wakes all of them. Returns the number woken (the
 * native back-end broadcasts, so its count is the number of
 * registered waiters, a superset of a capped request; every
 * waiter re-checks its word before proceeding, so waking extra
 * sleepers is a performance event, never a correctness one). */
uint32_t lt__notify(volatile uint32_t* addr, int32_t count);

#endif /* LITHOTHREAD_WAIT_H */
