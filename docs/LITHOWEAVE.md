# LithoWeave (Piece 4 of LithoRoot)

Status: PIECE 4 of the LithoRoot program, landed for Elfie's per-piece gate.
Nothing outside `lithoroot/` was touched. lkboot.js was read for the
freeze's open question 4 and not modified. Nothing is committed.

LithoWeave is the spawn / entry weave beneath LithoThread: it turns the
one frozen hosting primitive (`lithosys_thread_spawn`) into spawn, join,
detach, self identity, TLS destructor running, and thread exit for the
module. The end-state flow is LithoLibC pthread -> LithoThread ->
LithoWeave -> LithoSys. The weave deliberately contains no mutexes, no
condition variables, and no pthread-shaped API; those are LithoThread's
job (Piece 5).

Files:

- `include/lithoweave.h` ... the public header (strict C99, C++ safe).
- `src/lithoweave.c` ... the portable implementation.
- `src/lithoweave_atomic.h` ... internal atomic shim (internal header).
- `src/lithoweave_internal.h` ... the module / back-end seam contract.
- `src/lithoweave_native.c` ... the POSIX host back-end for tests.
- `tests/lithoweave_test.c` ... the host functional test.

## The spawn block and the cell protocol (both sides)

The frozen import takes exactly one word: `start_arg`. The weave defines
what it points at. The spawn block IS the thread's control block, in
shared memory, with a frozen prefix:

- word 0 (offset 0): the tid cell. `lw_spawn` stores the new tid there
  before the host is called; the host may read it (the native back-end
  does, and returns it; a JS host needs it to name the worker).
- word 1 (offset 4): the requested stack size in bytes, 0 for the host
  default. The frozen import has no stack parameter, so the request
  travels here; back-ends read it once at spawn.

The exit protocol, as implemented:

1. Module side (spawner): allocate the control block, store the tid in
   the cell, then call `lithosys_thread_spawn((uint32_t)block)`. The
   cell reads as the tid for the thread's whole life.
2. Host side: create the execution context so the module's exported
   `lithosys_thread_start(start_arg)` runs on the new thread. Return
   the tid (positive) or the negation of a `lithosys_err_t` code.
3. Module side (worker): the trampoline (below) runs the entry and the
   TLS destructors, marks the control block DONE, and RETURNS. The
   return is the completion signal to the host wrapper; the weave never
   stores 0 into its own cell and never notifies it.
4. Host side: after `lithosys_thread_start` returns, store 0 into the
   cell (sequentially consistent), then issue a notify on the cell's
   address (wasm: `Atomics.notify`; native back-end: condition variable
   broadcast), then retire the context.
5. Joiner side: `lw_join` claims the handle, then loops: load the cell;
   while it is nonzero, wait on it with the host wait primitive
   (wasm: `__builtin_wasm_memory_atomic_wait32`; native: the back-end's
   condvar registry). When the cell reads 0, the host's store is the
   release edge: the return value and final state, written by the
   worker before its return, are read under the weave lock, the
   control block is unlinked and freed, and the stored return value is
   handed back.

Writing the exit word is the host's half of the protocol by design: it
keeps exactly one writer per cell transition and makes "the host
reaped the worker" and "the thread finished" the same event, which is
what a joiner actually needs.

Spawn failure is total: if the host returns a negative code,
`lw_spawn` unlinks and frees the control block before returning the
code, so a refused spawn leaks nothing (proven by the test's
wrap-counted allocator oracle).

Detached threads have no joiner, so their control blocks are reclaimed
lazily: the trampoline leaves a DONE detached block in place (its cell
must stay valid for the host's store), and `lw__sweep`, run at the head
of spawn / join / detach, frees detached blocks that are DONE with a
zeroed cell. The application-visible cost is a bounded lag, never a
leak; the test proves the block is reclaimed by the next spawn.

## The trampoline, in words

`lithosys_thread_start(start_arg)` is the only function a host ever
calls into. Its sequence:

1. Widen `start_arg` to the control block; adopt it as the calling
   thread's current block (one thread-local pointer).
2. Arm the entry unwind pad (`setjmp`), then call the entry function
   with its argument and capture the return value. `lw_exit` on a
   spawned thread longjmps to this pad with the value preset, skipping
   whatever entry code remained.
3. Mark the block as exiting (this is what routes a later `lw_exit`
   call to the destructor pad instead of re-entering).
4. Arm the destructor unwind pad, then run the TLS destructor passes
   (below). An `lw_exit` from inside a destructor longjmps to this pad,
   abandoning the rest of the list, which is then freed without
   being run.
5. Under the weave lock, mark the block DONE (the return value was
   already stored). Clear the current-thread pointer.
6. Return to the host wrapper, which performs step 4 of the cell
   protocol. The trampoline does not terminate the OS / worker thread
   itself; returning is what ends it.

`lw_exit` on the main (or a foreign) thread runs that thread's
destructors the same way and then calls `lithosys_exit`: in the wasm
model the main thread's exit is the instance's exit.

## TLS destructors: passes and the cap

`lw_thread_atexit(fn, arg)` registers a destructor on the calling
thread's list (the hook LithoThread's keys and LithoAbi's
`__cxa_thread_atexit` will register through). At exit the weave runs
passes: each pass detaches the whole current list and runs it
head-first, meaning reverse registration order within the pass;
destructors registered during a pass land on the fresh list and run in
the NEXT pass. Passes repeat until the list is empty, capped at
`LW_TLS_DTOR_PASS_LIMIT` passes (8). Whatever remains after the cap is
freed WITHOUT being run.

Why whole-list passes rather than the pop-one loop libc++abi uses for
`__cxa_thread_atexit`: the batch pass makes the ordering contract
simple to state and to test (a known batch runs in exact reverse
order; late registrations form the next batch), at the price of
differing from libc++abi in one interleaving (registrations a, b, with
b registering c during the run: the weave logs b, a, c; pop-one would
log b, c, a). Both orders satisfy "a destructor registered during exit
still runs"; the weave's is documented here so LithoAbi can rely on it.

Why a cap at all: a destructor that re-registers itself (directly or
in a cycle) would otherwise hang the exiting thread and every joiner
behind it, forever, on a resource the host cannot preempt (a wasm
worker). The cap converts a hang into a documented drop. 8 passes is
far beyond any legitimate registration chain (real chains are 1 to 2)
and small enough that a pathological thread still exits promptly. The
test pins both behaviors: the a / b / c order case and a self-renewing
destructor that runs exactly 8 times and lets its joiner complete.

A destructor may also call `lw_exit`: that abandons the rest of the
phase (the un-run remainder of the in-flight pass and any fresh
registrations are freed without being run, by the landing pad the
longjmp returns to; the in-flight list is anchored in the control
block precisely so this drop cannot strand it) and the exit already
in progress keeps its original return value... an `lw_exit` value
passed mid-destructor does not replace the entry's. The test pins
this too: of destructors c, x, e in run order, x exits mid-pass, e
never runs, the join returns the entry's value, and the allocation
balance is exact.

## Decisions taken in this piece (forks)

- Allocation: control blocks and destructor nodes come from LithoLibC
  `malloc` over WaferHeap's default heap, not from a per-call
  allocator hook. The weave is in-module code; one allocator keeps
  ownership and accounting in one place, and WaferHeap statistics plus
  the test's wrap oracle are the leak evidence. No hook also keeps the
  frozen spawn path free of callback reentrancy.
- The stack-size request travels in word 1 of the spawn block (the
  frozen import has no room for it). Back-ends that cannot honor a
  size may ignore the word; the native back-end validates it through
  `pthread_attr_setstacksize` and reports `EINVAL` for absurd sizes.
- The host's positive return from `lithosys_thread_spawn` is success,
  full stop; the cell's tid is authoritative. The native back-end
  returns the tid it read from the cell, as a conforming host should.
- `lw_self()` materializes a self block for threads the weave did not
  spawn (main first of all) and marks it detached-by-construction: no
  host will ever zero its cell, so it is unjoinable and never swept.
  The first attacher in the process is the main thread, tid 1, via
  `lw_main_thread_init()`.
- Forward dependency, flagged for the toolchain piece: the trampoline
  uses `<setjmp.h>` (`lw_exit` semantics need a non-local exit that
  does not run the rest of the entry function). LithoLibC does not
  provide setjmp yet; the wasm form needs the toolchain's story for
  it (Lisp-style stack unwinding is not available; a SjLj or a
  table-driven scheme is a Piece 7-adjacent decision). The design only
  relies on `setjmp` / `longjmp` behaving per the C standard inside
  one thread.
- Status codes are `lithosys_err_t` end to end (the WASI-numbered set
  frozen in Piece 1), so a refusal from the host reaches the caller
  unmodified. `lw_join` returning `EINVAL` for unknown, detached,
  already-claimed, or self handles is the weave's own rule.
- Tid space: tids allocate from a wrapping uint32 counter starting
  at 2. After 2^31 spawns the counter crosses the sign boundary and
  `lw_join` / `lw_detach` refuse those handles (`tid <= 0`), so
  threads born in that span can be neither joined nor detached and
  their blocks are never reaped: a refusal-plus-leak span, not a
  corruption (the registry lookup's pointer-plus-tid double key
  still prevents any stale handle from aliasing a live thread).
  Accepted at realistic scale by the audit of 2026-10-28 (finding
  C-F8): no engine workload spawns two billion threads; a wider tid
  or a refusal at the sign boundary would close it formally if the
  span ever became reachable.

## The native back-end, and why its wait is test-only

`src/lithoweave_native.c` plays the host on POSIX systems so the weave
can be exercised today. It implements `lithosys_thread_spawn` over
`pthread_create` (detached at the pthread level; joining is the
weave's protocol, not pthread's) plus a wrapper that performs the
host's half of the cell protocol, and it implements the wait / notify
pair the weave calls:

- Wasm wait is a single instruction against shared memory
  (`memory.atomic.wait32`). POSIX has no portable userspace
  equivalent (C11 has no atomic wait; Linux futex is not portable),
  so the back-end keeps a small registry of mutex + condition
  variable slots keyed by cell address (64 slots, never retired,
  process-lifetime). Waiters check the cell value under the slot
  lock, which closes the missed-wakeup window: the notifier stores
  the 0 before it looks the slot up, so any waiter either sees the 0
  or is already blocked where the broadcast reaches it. A full table
  degrades to a 1 ms sleep loop rather than failing the wait.
- This machinery exists only because POSIX lacks the primitive; it is
  scaffolding shaped exactly like the protocol so that the wasm
  back-end (lkboot) can replace the file whole. Nothing in the weave
  proper knows it exists: `lw__cell_wait` / `lw__cell_notify_all` is
  the entire seam.
- One host accommodation, documented in the file: the frozen ABI
  narrows the block pointer to `uint32_t`. The back-end widens it
  back, so the test maps its WaferHeap arena below 4 GiB
  (`MAP_32BIT`) to make the round trip lossless. That is a property
  of the harness, not of the design: the frozen ABI is a wasm ABI,
  where linear memory offsets are 32-bit by construction.
- A test hook, `lw__native_test_fail_spawn(count)`, forces refusals
  (`EAGAIN`) so the spawn-failure path is testable on demand.

## Verification (host, gcc 13.3, this VM)

- Strict compile: `src/lithoweave.c` passes
  `-std=c99 -pedantic -Wall -Wextra -Werror`; the header is clean
  standalone in C99 and C++11 modes.
- Plain run: all 8 cases PASS (`LITHOWEAVE TEST: PASS`). Cases:
  16-thread spawn / join with distinct return values and a shared
  atomic counter; detach with completion proven by polling the cell
  itself, plus reclaim of the detached block at the next spawn;
  destructor order and multi-pass (`bac` for the a / b / c case);
  `lw_exit` from inside a destructor (the rest of the pass is
  abandoned and dropped, the entry's return value stands, balance
  exact; this case convicts the pre-fix landing pads, which leaked
  the un-run node); the runaway cap (exactly 8 runs, join
  completes); `lw_exit` from mid-entry (code after it never runs,
  join returns its value); forced spawn failure (error propagates,
  handle untouched, zero allocation drift, next spawn succeeds);
  self / main-thread rules.
- ASan + UBSan: all 8 cases PASS. Arrangement: the two thin shim
  files `src/libc/malloc.c` and `src/libc/errno.c` are compiled
  without sanitizer instrumentation, because an instrumented
  interposed malloc is called by the dynamic loader during the
  sanitizer's own interceptor initialization and faults there
  (observed directly in the ASan startup stack). Everything else,
  including WaferHeap and the back-end, is instrumented.
- TSan (-no-pie, under `setarch -R` for this VM's ASLR): all 8 cases
  PASS. The whole malloc path (`malloc.c`, `errno.c`, `waferheap.c`)
  is uninstrumented there for the same loader-init reason (TSan
  faults merely entering an instrumented function on that path);
  heap accesses are serialized by the injected pthread-mutex hooks,
  which TSan models. TSan reports 2 to 3 warnings, all one class:
  two owner memsets of a recycled heap block (the weave's control
  block memset, WaferHeap's calloc zeroing) with no TSan-visible
  synchronization edge between the ownership epochs, because TSan
  cannot see ownership transfer through an uninstrumented custom
  allocator. Both writes in every report are by the block's owner at
  the time and are ordered by the heap mutex. No warning touches the
  tid cell, return values, the registry, destructor lists, or any
  weave field; the protocol's synchronization (seq-cst atomics, the
  registry lock, the condvar pair) is fully instrumented and clean.
  Note: under heavy host load the TSan build can crawl (its
  instrumentation plus the spinlock); the test's cell polls use
  30 s wall-clock deadlines, and one warm-up stall of that class was
  observed during development on a loaded VM. The plain and ASan
  builds complete the whole suite in well under a second.

## What LithoThread (Piece 5) still owes on top

- The pthread-shaped API: `pthread_create` / `join` / `detach` as thin
  wrappers over `lw_spawn` / `lw_join` / `lw_detach`, plus thread
  attributes (the weave's descriptor is the seed).
- Synchronization primitives: mutex, condition variable, rwlock,
  once, barriers, built on LithoSys atomic wait / notify (the same
  primitive `lw_join` uses on the cell), with the self-cell accessor
  (`lw_self_cell`) giving condvar-style waits a per-thread word where
  the design wants one.
- TLS keys: `pthread_key_create` semantics, whose per-thread values'
  destructors register through `lw_thread_atexit` and inherit this
  piece's pass and cap behavior.
- The true-TLS swap for LithoLibC's errno accessor: Piece 3 left
  `__litholibc_errno_location()` returning a process-wide slot behind
  a function call precisely so LithoThread can re-point it at real
  thread-local storage without an ABI break.
- Stack provisioning policy: the weave passes the requested size to
  the host; who decides defaults and guards is LithoThread / LithoSys
  policy, not the weave's.

## Open question 4 from the LithoSys freeze: lkboot

Question: can lkboot's worker wrapper perform the frozen tid-cell
store + `Atomics.notify` on worker exit today (read from
`wasm/full/lkboot.js`, 4,479 lines)?

Answer: no, not today. lkboot's worker path is still vendor-shaped
end to end:

- Worker entry (`startThread`, about line 4376) initializes from the
  vendor pthread struct (stack limits, `_emscripten_thread_init`,
  `_emscripten_tls_init`, mailbox wait) and enters the thread through
  the indirect function table with the vendor start routine and arg.
  There is no `lithosys_thread_spawn` import and no call to an
  exported `lithosys_thread_start`.
- On entry return, the wrapper calls the in-module vendor export
  `_emscripten_thread_exit(result)`, which performs detach / exit
  state, the futex wake for `pthread_join`, and TLS cleanup INSIDE
  the module against vendor pthread-struct fields. The JS wrapper
  never stores 0 into a tid cell and never issues `Atomics.notify`
  for thread exit. A separate import, `_emscripten_thread_cleanup`,
  posts `{cmd: 'lk-done', pthread_ptr}` so the page can recycle the
  worker; that recycling signal is keyed to the vendor pthread struct,
  not to any cell.
- Page side, creation maps the vendor `__pthread_create_js` to
  `lkSpawnThread(pthread_ptr, startRoutine, arg)` (line 1178): a
  pool of pre-warmed workers, a soft cap (spawning past the pool is
  allowed with a warning; `EAGAIN` only on Worker construction
  failure or a single-agent context), and `{cmd: 'lk-run', ...}`
  messages. In-worker creates are proxied to the page through
  `lkSpawnRequest`. None of this knows the weave's spawn block.

The capability is present: lkboot already builds fresh memory views
per call (`lkI32()` / `lkU32()`, lines 141-142, growth-safe) and
already uses `Atomics.store` + `Atomics.notify` for other slots
(lines 1007-1008, 1264-1265, 1301-1302, 1343-1344). What must change
later (Piece: the wasm back-end landing; NOT done here):

1. A `lithosys_thread_spawn(start_arg)` import in the page's import
   object: read the tid from cell word 0 at `start_arg`, grant a pool
   worker under the same soft-cap / `EAGAIN` policy `lkSpawnThread`
   uses, post a NEW run message carrying `start_arg` (for example
   `{cmd: 'lk-litho-run', start_arg}`), and return the tid (or the
   negated error code on refusal).
2. A worker-side handler for that message that calls the exported
   `lithosys_thread_start(start_arg)` and, when it returns, performs
   `Atomics.store(i32view, start_arg >> 2, 0)` followed by
   `Atomics.notify(i32view, start_arg >> 2)` BEFORE posting any
   done / recycle message, so the page only retires the worker after
   joiners have been woken. The vendor init exports
   (`_emscripten_thread_init`, `_emscripten_tls_init`, stack-limit
   setup from the pthread struct) must not be reused for this path;
   the weave's trampoline is the whole entry.

## Gate notes (Elfie, 2026-10-28)
- Gate re-run by Elfie from the documented recipe (native back-end compiled
  WITHOUT the LithoRoot include dir, per the file's own header; the rest
  with it): plain and ASan+UBSan both print LITHOWEAVE TEST: PASS, 8/8.
  A flat -Iinclude build of the back-end fails on <stdlib.h> resolving to
  the seeded LithoLibC header... a build-recipe fact, not a code defect;
  the recipe in this doc is the authority.
- ACCEPTED DIVERGENCE, recorded: TLS destructor passes detach the whole
  list per pass, so a destructor registered mid-pass runs AFTER the rest
  of its pass (b, a, c) where libc++abi/glibc pop-one ordering gives
  (b, c, a). Snapshot semantics are simpler to audit, no engine code
  depends on mid-exit registration order, and the cap behaviour is
  documented above. Revisit only if a real consumer ever cares.
- Forward debts this piece names: <setjmp.h> does not exist in LithoLibC
  yet (the trampoline uses it; the wasm setjmp story is a toolchain-piece
  decision), and lkboot needs the two page-side changes listed in the
  lkboot answer before the wasm weave can run (post-purge workstream).
