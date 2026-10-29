# LithoThread: the pthread-shaped thread API (LithoRoot Piece 5)

Status: landed, gate copy. Files: `include/pthread.h` (285 lines),
`src/lithothread.c` (1,315), `src/lithothread_errno.c` (60),
`src/lithothread_native.c` (215), `src/lithothread_heap.c` (63),
`src/lithothread_atomic.h` (134), `src/lithothread_internal.h`
(153), `src/lithothread_wait.h` (58), `tests/lithothread_test.c`
(1,030). Total 3,313 lines including the test.

LithoThread is the API the engine's code will actually call:
POSIX-shaped threads, mutexes, condition variables, rwlocks,
once, and TLS keys, implemented over LithoWeave (Piece 4) plus
one platform primitive the weave does not provide, an atomic
wait/notify on a 32-bit word. Architecture studied from musl's
pthread design (MIT licensed) and written fresh: sequence-count
condition variables, three-state mutex words, generation-stamped
key slots. Nothing is copied; the shapes are the ones the
literature settled because they are provable, and the proofs
below are written for this code.

## The wait/notify seam

Everything blocking in this piece reduces to two operations,
declared in `src/lithothread_wait.h`:

```c
int      lt__wait32(volatile uint32_t* addr, uint32_t expected,
                    int64_t timeout_ns);   /* <0 = forever */
uint32_t lt__notify(volatile uint32_t* addr, int32_t count);
```

Return codes deliberately mirror the wasm `memory.atomic.wait32`
instruction's own results (0 woken, 1 not-equal, 2 timed out).
Two providers:

- **wasm** (`lithothread.c`, under `__wasm__`): the clang
  builtins `__builtin_wasm_memory_atomic_wait32` /
  `__builtin_wasm_memory_atomic_notify`. These are the
  instruction-level primitives LithoSys's freeze assumed; no
  import is involved, so nothing in the frozen host surface
  changes. NOT YET COMPILE-VERIFIED: this VM has no clang and no
  wasm toolchain (the toolchain is a later piece, and the game
  build is suspended), so the exact builtin spellings are the
  first thing that piece should check. The seam is one header
  and about ten lines precisely so a spelling fix lands in one
  place.
- **native test** (`lithothread_native.c`): a 256-slot registry
  of host pthread mutex/cond pairs keyed by word address, the
  same pattern as Piece 4's back-end. The waiter re-checks the
  value under the slot lock before sleeping, so a notify that
  lands between the caller's value check and the wait is not
  lost. Notify broadcasts to the slot's cond regardless of the
  requested count (a superset wake; every waiter in this piece
  re-checks its value or predicate in a loop, so extra wakeups
  are only wasted cycles). If the table ever fills, waits
  degrade to a 1 ms poll on the value, so the registry delays,
  it never deadlocks. The same file supplies
  `lithosys_clock_now_ns` over `clock_gettime` for the test
  link (the weave's back-end does not provide the clock).

All waits in the piece are loops over these two operations with
the value reloaded each iteration, which is what makes the
superset wake and the poll fallback correctness-neutral.

## Mutexes

One state word, the three-state protocol from Drepper's futex
paper: 0 unlocked, 1 locked uncontended, 2 locked and possibly
waited on. Lock fast path is a single CAS 0 to 1. On contention
the word is exchanged to 2 and the loser sleeps with
`lt__wait32(word, 2)`. Unlock decrements; only when the old
value was 2 does it store 0 and notify one waiter. There is no
spin loop before waiting: the engine's critical sections are
short but its thread counts are not tiny, and a fixed design
with no tuning constants was worth more here than a spin that
helps one benchmark. (Flagged as a deliberate fork; a bounded
spin is a five-line change behind the same seam if profiling
ever asks for it.)

Flavors live in three more words of the public struct
(type / owner tid / recursion count). Static initializers pin
the musl-compatible encoding (NORMAL 0, RECURSIVE 1,
ERRORCHECK 2), so an all-zero mutex is a normal mutex and
`PTHREAD_MUTEX_RECURSIVE_INITIALIZER` is `{0, 1, 0, 0}`.
Flavor answers: recursive relock by the owner increments the
count; errorcheck relock answers EDEADLK (as does timedlock);
errorcheck/recursive unlock by a non-owner answers EPERM;
trylock on a mutex the caller already holds answers EBUSY for
both checked flavors. Unlocking an unlocked NORMAL mutex
answers EINVAL here where glibc silently returns success on
undefined behavior; ours is the diagnosed behavior, chosen so
engine bugs surface. Destroy poisons the type word, so use
after destroy is EINVAL rather than a resurrection.

Owner bookkeeping order matters and is commented at the site:
the owner word is cleared BEFORE the state word is released,
never after, because a late clear could erase a new acquirer's
just-stored owner.

`pthread_mutex_timedlock` takes its deadline on CLOCK_REALTIME
(POSIX) and converts per iteration through `lithosys_clock_now_ns`;
a timeout leaves the word at 2, which the holder's unlock path
already handles.

## Condition variables: the construction and the no-miss argument

The public cond is two words that matter: a sequence counter
and a waiter count. There is no internal mutex and no waiter
list. The dance:

- **Waiter** (holding the user mutex): atomically increments
  the waiter count, snapshots the sequence, releases the user
  mutex, then sleeps with `lt__wait32(seq_addr, snapshot)`,
  looping until the sequence differs from the snapshot or the
  deadline passes. It decrements the count and re-acquires the
  user mutex before returning, always, including on timeout.
- **Signaler**: atomically increments the sequence, then, if
  the waiter count is nonzero, notifies. Signal and broadcast
  are the same operation here (see the herd note below).

Why a wakeup cannot be missed. Suppose a waiter's predicate
becomes true and the signaler signals. Two orderings exist for
the signaler's sequence bump relative to the waiter's snapshot:

1. The bump precedes the snapshot. Then either the waiter's
   predicate check (which POSIX requires under the mutex, and
   the signaler must also hold the mutex to change the
   predicate) already saw the true predicate and it never
   waits, or its snapshot postdates the bump and the value it
   sleeps on is already stale... precisely, if the bump
   happened before the snapshot, the snapshot equals the new
   sequence, but the predicate re-check that follows any wake
   is not the ordering that saves us here: what saves us is
   that the predicate itself is checked under the mutex AFTER
   the signaler's change became visible, so the waiter exits
   its predicate loop without sleeping at all. The snapshot
   and the sleep only happen for a predicate observed false
   under the mutex, which orders the snapshot BEFORE any
   later predicate change: a signaler cannot change the
   predicate until it acquires the mutex the waiter still
   holds while snapshotting.
2. The bump follows the snapshot. The waiter is either already
   asleep on the snapshot value (the notify wakes it: the
   address holds a different value now) or between snapshot
   and sleep, in which case `lt__wait32`'s not-equal return
   fires immediately because the word no longer equals the
   expected value. On wasm that check is the instruction
   itself; in the native provider it is the re-check under the
   slot lock.

The waiter count is what licenses the signaler to skip notify
entirely when nobody waits (a value-only check would race with
a waiter between snapshot and sleep; the count is incremented
BEFORE the snapshot, so a signaler that reads a stale zero is
still safe: its bump is then ordered before the snapshot, case
1). The count is also the honest price: two extra atomic ops
per wait.

**The herd decision.** `pthread_cond_signal` wakes ALL waiters
here, not one. With a bare sequence word the notifier cannot
address a specific waiter, and waking one arbitrary waiter
whose predicate is false spends the wakeup while the waiter
the signal was meant for stays asleep: a missed wakeup wearing
a costume. The herd costs extra wakeups and predicate
re-checks; the proof above has no hole. Waiter counts in the
engine are small. This is the piece's most deliberate fork and
it is flagged for the gate: if a future profile shows herd
churn hurting, the fix is a waiter queue, which is a different
(and larger) construction, not a tweak.

**Clocks.** The wait deadline runs on the cond's own clock
(condattr setclock, default CLOCK_REALTIME; CLOCK_MONOTONIC
supported). Care with numbering: LithoSys freezes MONOTONIC as
id 0 and REALTIME as id 1, the opposite of POSIX; every clock
read funnels through one helper that maps. On timeout racing a
signal, the signal wins: if the sequence moved, timedwait
returns 0 even past the deadline. Spurious and herd wakeups
with an unmoved sequence re-sleep on the remaining time.

**Recursive mutexes in cond waits.** POSIX leaves wait on a
multiply-locked recursive mutex undefined. LithoThread gives
it definite semantics: the wait releases ALL recursion levels
and restores the count on return, so the pattern works like
the single-level case. Ownership is verified for the checked
flavors (wait without holding is EINVAL); a normal mutex
cannot be verified, as everywhere.

## Rwlocks

One lock word: bit 31 is the writer flag, bits 0..29 the reader
count, plus two sequence words (readers sleep on one, writers
on the other; a releasing writer bumps both, the last departing
reader bumps the writer sequence). Reader-preference by design:
new readers barge past a waiting writer, so read-mostly
structures never convoy, and a writer can starve under a
continuous read flood. That is documented POSIX-acceptable
behavior and the right default for the engine's read-heavy
tables; a writer-preference variant would need a gate word
and is not built. Write-lock upgrade while holding a read
lock deadlocks (undefined by POSIX, diagnosed here only in
the note, not in code). Timed variants exist and share the
blocking core.

## Once

State word 0 (untouched), 1 (running), 2 (done). The CAS
winner runs the routine with nothing held and publishes 2 with
a notify-all; losers sleep on the word. If the routine calls
pthread_exit, the state sticks at 1 and later callers sleep
forever: POSIX solves this with cancellation cleanup handlers,
which this piece does not have (see the cut list). Documented
rather than papered over.

## Threads, records, and reclamation

`pthread_t` IS `lw_thread_t` (the weave's two-word handle), so
a LithoThread thread and a weave thread are the same object and
the layers can never disagree about identity. Each thread also
carries an `lt_rec_t` (return value, exit pad, errno slot, key
values, flags) in a registry under a small test-and-set spin
lock that is never held across a wait, an allocation, or user
code.

One integration point cost a debugging cycle and is recorded
so nobody repeats it: **the weave spawns joinable only.**
Detachment at the weave level is a separate `lw_detach` call.
`pthread_create` with a detached attr therefore calls
`lw_detach` immediately after a successful spawn, while the
handle is still unpublished (the pattern the weave's own test
uses). Omitting it leaked the weave control block for every
attr-detached thread; the test's balance oracle convicted it
(see the test log below).

Reclamation protocol. Joinable records are freed by the joiner
after `lw_join`. Detached records are reaped lazily, and the
exit-vs-detach race is settled by two atomics on the record
(`exit_phase_done`, `retire_claimed`): the exit runner stores
exit_phase_done after running key destructors, then claims the
retire CAS if the record is detached; `pthread_detach` on an
already-finished thread observes exit_phase_done with a zero
tid cell (which orders it after the runner) and tries the same
CAS. Exactly one side wins the CAS and frees or dead-lists the
record; dead-listed records are swept at the next
`pthread_create` / `pthread_detach` entry, the same lazy
bargain the weave strikes for its control blocks. Attached
records (threads LithoThread did not spawn: the main thread,
foreign host threads) live for the process; they are how errno
and keys work on threads we did not create.

`pthread_exit` on a spawned thread unwinds through a setjmp
landing pad in the trampoline (host `<setjmp.h>`; the wasm
SjLj story is the toolchain piece's decision, the same forward
debt Piece 4 recorded). On the main or a foreign thread it runs
key destructors directly and hands over to the weave's exit,
which ends the instance; POSIX would keep the process alive
while other threads run, and an instance-model process cannot
honor that, so the divergence is documented, not hidden.

## TLS keys

A fixed table of 128 slots (PTHREAD_KEYS_MAX), each with a
destructor pointer and a generation counter, guarded by a
spin lock for create/delete and read lock-free on the fast
path. Per-thread values live in a lazily allocated array on
the thread's record, each entry stamped with the slot's
generation: when a key is deleted and its slot later reused,
stale values from the previous incarnation carry the old
generation, read as NULL, and never meet the new destructor.
Key destructors run from a single runner registered once per
record into LithoWeave's registry, so they ride the weave's
exit phase and its documented pass semantics (accepted
divergence recorded at Piece 4's gate: destructor passes
snapshot the list per pass, so cross-registry ordering can
differ from libc++abi's pop-one order; key destructors are
within this piece's own registry and pass in key order here).
Within the runner, POSIX's iteration is honored: passes repeat
while any destructor set a new value, capped at
PTHREAD_DESTRUCTOR_ITERATIONS (4); values still set after the
cap are dropped, which POSIX permits.

One accepted wraparound, recorded by the audit of 2026-10-28
(finding C-F7) rather than left silent: the generation counter
is a uint32 bumped once per `pthread_key_delete`. After 2^32
delete cycles on a single slot, a value held by a still-living
thread across every one of them would carry a matching
generation again and be returned by `pthread_getspecific` and
destroyed at exit under the new incarnation's destructor. The
span needs four billion deletes plus a thread holding a value
throughout; it is not reachable at realistic scale, and no code
guards it. Widening the generation, or retiring a slot
permanently when its counter wraps, would close it formally.

## errno: the per-thread slot and the link rule

The real `__litholibc_errno_location()` lives in
`src/lithothread_errno.c` and returns a pointer into the
calling thread's record. **Link rule (the coordination with
Piece 3, solved without editing its files): a link that
includes LithoThread EXCLUDES `src/libc/errno.c`; a link
without LithoThread keeps it. Exactly one strong definition of
the symbol per link, always.** Weak symbols were rejected:
MSVC has none, and archive-member weak resolution on the wasm
linkers is murky enough that a rule a human can state beats a
mechanism three toolchains interpret three ways.

One refinement keeps Piece 3's sanitizer lesson intact: the
accessor is inert until armed. `__litholibc_errno_arm_tls()`
(startup code calls it once the default heap exists and the
main thread is attached) flips a flag in the errno TU; before
that, the accessor returns a plain fallback slot and never
enters LithoThread, so loader-time and sanitizer-init errno
writes stay on the safe path Piece 3 established, and a
LithoThread-linked process still works if startup forgets to
arm (errno is then process-global until it does). Attaching a
record from inside the accessor is recursion-guarded: attach
allocates, allocation failure writes errno, and the nested
accessor call during an attach in progress answers with the
fallback slot instead of recursing.

## WaferHeap glue and link order

`src/lithothread_heap.c` exports `lithothread_heap_options()`,
returning a `wh_options_t` whose lock/unlock hooks are wired to
one process-wide, statically initialized NORMAL mutex. The
caller fills in the grow fields and passes the options to
`wh_heap_create`. One global mutex (rather than per-heap) is
forced by the hook shape: the hooks receive only the
`lock_user` pointer, which must be valid before any per-heap
state exists; and the normal flavor's lock path is pure atomics
(no tid lookup, no allocation), which is what makes it safe
inside the spawn path, where heap locks nest with thread
creation. Link-order story: linking `lithothread_heap.o` plus
`lithothread.o` is what makes the allocator thread-safe; a
program that links WaferHeap alone keeps the NULL-hook
single-threaded heap it had in Piece 2. Nothing about WaferHeap
itself changed.

## The cut list, with reasons

- **Cancellation**: a deferred-cancel design needs async
  cancellation points woven through LithoLibC's blocking calls
  and async cancel is unsafe by construction. The engine's
  threads stop cooperatively (flag + signal), the pattern the
  codebase already uses. Not built; `pthread_cancel` does not
  exist here, so accidental use fails at link, loudly.
- **Signals**: per-thread signal masks and delivery belong to
  a host surface LithoSys deliberately does not have (no signal
  imports exist to build on). Out of scope for the instance
  model, not just this piece.
- **Robust mutexes**: owner-death recovery needs the host to
  report thread death into shared state; the weave's cell
  protocol reports it to a joiner, not to a mutex word, and
  faking it would produce recovery semantics nobody can test.
- **Priority scheduling / SCHED policies / priority ceiling
  and inheritance mutexes**: wasm threads have no priority
  channel to the host scheduler, and the native side would
  inherit host semantics the module cannot share. Attr setters
  for these do not exist; detach state and stack size are the
  only thread attrs, because they are the only ones both
  targets honor identically.
- **Process-shared objects**: mutex/cond/rwlock pshared attrs
  accept and store PTHREAD_PROCESS_PRIVATE; SHARED is answered
  ENOTSUP at init (not silently degraded), because instance
  memory is not shared between processes and a quiet no-op
  would be a lie with a lock around it.
- **pthread_atfork**: there is no fork in the instance model
  and no fork in LithoSys. The function does not exist.
- **Barriers and the standalone spinlock type**: no engine
  consumer today; both are small additions behind the same
  seam when one appears. Not built, deliberately: every
  primitive in the piece earns its proof.

Public-header self-defense worth knowing: `pthread.h` supplies
`struct timespec` / `clockid_t` / CLOCK_* fallbacks under
guards when the host `<time.h>` withheld them (strict C99
consumers get neither from glibc), so the header compiles
standalone in C99, C11, and C++11 modes. The future LithoLibC
`<time.h>` retires that block. No `restrict` appears in the
header (the Piece 3 C++ lesson).

## Test evidence (host, weave native back-end)

`tests/lithothread_test.c`, 13 cases: create/join exactness
with balance oracle, detach (attr and post-create, with lazy
reap convergence), mutex contention (8 threads x 50k guarded
increments, exact 400,000), mutex flavors (recursive depth,
errorcheck EDEADLK, foreign unlock EPERM, timedlock ETIMEDOUT
with elapsed bounds), condvar bounded queue (1 producer /
1 consumer, 10,000 FIFO handoffs, sum and order checked),
broadcast herd release, cond timedwait (monotonic clock,
ETIMEDOUT with bounds, past deadline, malformed abstime),
rwlock (writer exclusion, concurrent readers, writer waits for
last reader), once under 8-way race, once whose routine calls
pthread_exit (review M4: the once rolls back to fresh and wakes its
waiters; a C++ exception thrown through the routine, or a direct
lw_exit, is NOT rolled back and still leaves the once running), keys (per-thread values,
destructor passes including a destructor that re-arms another
key, delete + generation inertness), per-thread errno (three
threads holding 111 / 222 / 333 concurrently), heap glue churn
(8 threads x 5k malloc/realloc/free over a glue-locked heap,
exact live-block and free-byte equality after), pthread_exit
(value, unreachable code, key destructor still runs).

The link uses a **symbol prefix**: every public pthread_* name
is compiled as lt_pthread_* in the library and test TUs. This
is load-bearing, not cosmetic: the native back-ends are
statically linked into the same executable and call the HOST's
pthread functions for their registries, so an unprefixed link
would interpose the back-end's own pthread_create into ours
and recurse. Under the prefix the code under test is the
production code with only its symbol names changed (Piece 3's
sanitizer prefix, promoted to every run). The leak oracle is
the linker wrap counter, extended to calloc/realloc (the
library allocates records with calloc; an oracle that wraps
only malloc/free reads every record as a leak, and did, until
it was taught all four entry points).

Results at landing:

- **Plain**: 13/13 PASS.
- **ASan + UBSan**: 13/13 PASS, no reports. (malloc.c and
  lithothread_errno.c compiled uninstrumented per the Pieces
  3/4 loader-path recipe.)
- **TSan**: 13/13 PASS with `TSAN_OPTIONS=halt_on_error=0`
  under `setarch -R`, 10 warnings, every one the
  cross-lifetime recycled-address class whose middle lives in
  the uninstrumented allocator (summaries at the calloc memset
  store; zero warnings on mutex/cond/rwlock state, key tables,
  or user data; the annotations teaching TSan the mutex and
  cond edges are compiled in under `__SANITIZE_THREAD__`). One
  TSan-specific build define, `LT_NO_HOOK_DEFAULT_HEAP`, drops
  the glue lock from the DEFAULT heap in the TSan build only:
  glibc calls the interposed free from ld.so's TLS teardown on
  an exiting thread, and with the glue lock present that path
  enters an instrumented mutex while TSan recycles the dying
  thread's trace and faults inside TSan's own runtime
  (stack captured during development; the plain and ASan
  builds run the hooked default heap in full, and the glue
  case still exercises the hooks under TSan on its own heap).

Two genuine bugs were caught by this suite during development
and fixed before landing: the missing `lw_detach` after
attr-detached spawns (control block leak, convicted by the
balance oracle), and a test expectation that encoded a race
(second detach answering ESRCH instead of EINVAL once the exit
path wins reclamation; POSIX leaves it undefined and the test
now accepts either refusal).

## What the wasm build still needs from the toolchain piece

1. Verify the atomic wait/notify builtin spellings used behind
   `lt__wait32` / `lt__notify` (`__builtin_wasm_memory_atomic_wait32`,
   `__builtin_wasm_memory_atomic_notify`) against the stock
   clang the toolchain piece adopts, including the shared-memory
   and atomics target features the module must be built with.
2. The `<setjmp.h>` decision (Piece 4's debt, shared here):
   the trampoline's pthread_exit pad uses host setjmp on
   native; wasm needs the toolchain piece's SjLj ruling.
3. LithoLibC's own `<time.h>` (Piece 6 territory), retiring the
   header fallback block described above.
4. The two lkboot page-side changes Piece 4 specified (worker
   start message + cell store/notify) are what make the weave,
   and therefore everything here, run on device; unchanged by
   this piece.

## Honest remaining risks

- The condvar herd is a real cost under many-waiter,
  single-predicate workloads; chosen for provability, flagged
  above, and the first thing to revisit if a profile convicts
  it.
- Reader-preference rwlocks can starve a writer under a
  sustained read flood. Known, documented at the header.
- `pthread_exit` from the main thread ends the instance
  (documented divergence).
- Records of attached foreign threads are process-lifetime by
  design; a host that churns short-lived foreign threads
  through errno/keys grows the registry monotonically. Bounded
  by the host's own thread bookkeeping in practice; noted.
- A mutex or cond destroyed while another thread is between
  its value check and its wait is undefined, as in POSIX; the
  poison words turn most such bugs into EINVAL noise rather
  than silence, but the seam itself cannot see a sleeper on a
  destroyed object.
- The native wait registry is a test scaffold, not a product
  component; its 256-slot contention behavior says nothing
  about the wasm path, whose wait is one instruction.
