/* lithoweave_internal.h ... the weave / back-end seam (internal).
 *
 * LithoWeave proper (lithoweave.c) is platform-neutral except for
 * two primitives that only the platform side can supply:
 *
 *   lw__cell_wait: block while a tid cell still holds an expected
 *     value. On wasm this is the memory.atomic.wait32 instruction
 *     and lithoweave.c provides it itself under __wasm__. Everywhere
 *     else the platform back-end provides it (the POSIX test
 *     back-end in lithoweave_native.c uses a mutex / condvar
 *     registry keyed by the cell, because portable native C has no
 *     futex; the module-visible protocol is identical either way).
 *
 *   lw__cell_notify_all: wake every waiter on a tid cell. Only the
 *     HOST side of the exit protocol calls this: after the thread's
 *     start function returns, the host wrapper stores 0 into the
 *     cell and then notifies. The module never notifies its own
 *     cell; on wasm both the store and the notify are done by the
 *     page layer (JS Atomics), so no C definition of this function
 *     exists in a wasm build. The native back-end defines it for
 *     its wrapper's use.
 *
 * The wait contract is futex-shaped: the caller passes the value
 * it just loaded from the cell; the wait returns promptly if the
 * cell no longer holds that value, and otherwise blocks until a
 * notify or a spurious wakeup. Callers always re-load and loop,
 * so spurious wakeups are harmless by construction.
 */
#ifndef LITHOWEAVE_INTERNAL_H
#define LITHOWEAVE_INTERNAL_H

#include <stdint.h>

/* Offsets of the two frozen spawn block words (see lithoweave.h).
 * The control block struct in lithoweave.c is laid out to match
 * and asserts it at compile time. */
#define LW_SPAWN_CELL_OFFSET 0u
#define LW_SPAWN_STACK_OFFSET 4u

/* Block while *cell == expected; returns when the cell may have
 * changed (notify, value change, or spuriously). */
void lw__cell_wait(const volatile uint32_t* cell, uint32_t expected);

/* Wake all waiters on cell. Host side of the protocol only. */
void lw__cell_notify_all(const volatile uint32_t* cell);

#endif /* LITHOWEAVE_INTERNAL_H */
