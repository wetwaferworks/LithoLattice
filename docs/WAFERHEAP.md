# WaferHeap design note

Status: PIECE 2 of the LithoRoot program, landed for Elfie's
per-piece gate. Files: `include/waferheap.h` (public surface),
`src/waferheap.c` (implementation, about 710 lines),
`tests/waferheap_test.c` (host functional test). Piece 1's LithoSys
freeze is the only dependency of the design: on wasm, heap growth
reaches the host exclusively through the grow callback described
below, which is where `lithosys_memory_grow` gets wired. Nothing in
the existing engine or game source was touched, and no other
LithoRoot piece was started.

## The structure, as implemented

WaferHeap is a TLSF allocator (two-level segregated fit), written
fresh from the design: segregated free lists indexed by a two-level
bitmap mapping, boundary tags for immediate coalescing, and a search
whose cost is a bounded number of bit operations regardless of how
many blocks the heap holds. Malloc and free are O(1) in the strong
sense the engine wants for its determinism story: no list walks, no
history-dependent scans anywhere on the hot paths.

Blocks. Every block carries a two word header: a pointer to the
physical predecessor block, and the block size with flag bits packed
into the low bits that alignment leaves spare (bit 0: this block is
free; bit 1: the physical predecessor is free). Payload follows the
header; free blocks store their segregated list links in the first
two payload words, which sets the minimum payload at two words and
the minimum block at four words (16 bytes on 32-bit targets, 32 on
64-bit; the header overhead charged to every block is two words).
One deliberate
divergence from the leanest TLSF variants: they squeeze the header
to one word by overloading fields; we keep two words so the boundary
tag logic (predecessor pointer plus previous-free bit) stays direct
and auditable. The cost is one extra word per block; the benefit is
that coalescing needs no footer reads and no special cases.

Each adopted extent is a region: a small region descriptor at its
base, then blocks back to back, ended by a zero-size sentinel header
so physical walks always terminate inside the extent. Regions form a
list; a pointer is validated against the region list before any free
or resize touches it, which is what makes freeing a foreign pointer
a safe no-op instead of corruption.

Mapping. Sizes below the small limit (16 alignment steps: 128 bytes
on 32-bit, 256 on 64-bit) map linearly into class 0, one bucket per
alignment step. Above it, the first level is the power of two of the
size and the second level subdivides that range into 16 equal
subranges. Search rounds the request up to the next subrange
boundary, then consults the bitmaps: one word for first-level
occupancy, one word per first-level class for second-level
occupancy. The bit scans are portable C99 binary reductions: no
compiler intrinsics, no lookup tables, identical behavior under
MSVC, gcc, clang and the wasm toolchain.

Freeing coalesces immediately, both directions: the previous-free
bit plus the predecessor pointer find the block before, the size
finds the block after, and any free neighbor is unlinked, merged
and reinserted in O(1). The churn test ends with a fully drained
heap reporting exactly one free block spanning the region, which is
the observable proof that coalescing is complete.

Realloc works in place when geometry allows: shrink splits the tail
off (absorbing a free successor into it), growth absorbs a free
physical successor before it ever copies. The allocate-copy-free
path remains for the rest, including over-aligned blocks.

Over-aligned allocation (alignment above the two-word guarantee)
over-allocates, aligns the payload start inside the block, and
stores the distance back to the header, tagged with the low bit, in
the word just before the returned pointer. The word before a plain
payload is the block's size word, whose free bit is 0 on every live
block, so the tag distinguishes the two shapes without a flag day.
Free, realloc and usable-size all decode through the same path.
The tag distinguishes the two payload shapes; it is not, by
itself, evidence that a pointer is live. Since the audit fix of
2026-10-28 (finding A-1) the decode trusts nothing it reads: the
candidate the tag arithmetic produces must prove itself against
the physical chain before any heap state is touched. It must sit
in the same region on the alignment grid, its size must reach at
least the minimum block and end no later than the region's
sentinel, the payload offset must fall inside it, its successor's
prev_phys must point back at it, and its predecessor must be NULL
exactly when it is the region's first block and otherwise end
exactly where it begins. Interior, stale and double frees fail
the proof and are rejected as no-ops.

## Size-class parameters, and why

- Second level: 2^4 = 16 subranges per power of two. This bounds
  the worst-case rounding waste inside a range to under 1/16 of the
  range size, and keeps the per-heap index compact: at most 57
  first-level classes on a 64-bit target (26 on 32-bit), each with
  16 list heads and one bitmap word. The context is about 7.6 KB on
  64-bit hosts and under 2 KB on wasm32, most of it the list heads.
- Alignment: two words guaranteed (8 bytes on 32-bit, 16 on
  64-bit), so every C99 scalar type is served by the plain family
  and 64-bit hosts get the 16-byte alignment SIMD payloads expect.
- Growth slack: a grow request asks for the need plus half again.
  On wasm each granted page range costs the host real memory; the
  slack halves the number of growth round trips for a heap filling
  steadily, while staying small enough that a tight host cap still
  grants most requests in part or refuses them cleanly.

## The grow contract

WaferHeap itself never grows anything. When an allocation cannot be
served it invokes the options' grow callback once, with the lock
held, asking for the shortfall plus slack. The callback returns a
fresh extent, which the heap adopts as a new region (aligning the
base up internally first, so a callback slicing a larger buffer does
not have to track our alignment), or NULL to refuse, in which case
the allocation returns NULL and the heap is untouched. A short grant
is adopted even when it cannot serve the pending request; the
request fails, the memory stays useful. There is no grow loop: one
attempt per allocation, so failure behavior is bounded and
explainable.

On wasm the callback is a thin shim over `lithosys_memory_grow`:
compute pages from the shortfall, ask the host, and on success hand
back the extent at the old memory end (growth is contiguous there in
practice, but WaferHeap neither needs nor assumes contiguity, which
is why one mechanism covers wasm and native alike). A host cap
refusal arrives as LITHOSYS_ENOMEM, the shim returns NULL, and the
allocation fails soft, exactly the shape the Piece 1 freeze promised
("WaferHeap needs the refusal, not the number"). On native hosts the
same heap runs over malloc or mmap extents behind the same callback.

Memory is never handed back. wasm linear memory cannot shrink, and
a native backend that wants its extents back has no hook here by
design: regions are adopted for the life of the heap.

## The lock hook

`wh_options_t` carries lock and unlock function pointers plus a user
pointer; NULL means no locking. Every public entry point brackets
its work with the pair, the grow callback runs under the lock (and
so must not re-enter the heap), and the internal helpers assume the
lock is held. LithoThread's later piece can therefore put one heap
under a real mutex, or the engine can run per-thread heaps unlocked,
without an API change. The default heap pointer for the libc layer
is set once before threads start and is deliberately not itself
synchronized; the calls through it take the heap lock as usual.

## Determinism note

For a fixed sequence of calls (same sizes, same order, same growth
answers) the allocator returns the same addresses on every platform
of the same pointer width: the mapping, the LIFO list discipline and
the coalescing rules leave no room for environment influence. The
standing rule for callers, also stated in the header: addresses are
a product of allocation history, and allocation history is not
simulation state. Sim code must never order by pointer, hash by
address, or iterate in allocation order where the result is
sim-visible. The allocator states the rule and spends no cycles
enforcing it.

## Deliberately left out

- No per-thread caches and no lock-free fast path. Complexity and
  nondeterminism risk the engine does not need; one lock per heap
  is the whole threading story until measurements say otherwise.
- No statistics beyond `wh_stats_t`: byte and block counters kept
  incrementally (free), plus largest-free computed by a list scan
  when asked (diagnostic only, never on a hot path).
- No secure zeroing, no quarantine, no poisoning, no guard pages.
  Debugging aids belong in test builds of the caller, not in the
  one allocator every build ships.
- No memory return, no region removal (see the grow contract).
- No size classes beyond the TLSF mapping: no exact-fit small
  object pools. If a future profile shows a hot fixed-size stream,
  it gets its own pool built on top, not knobs in here.

## Decisions taken inside the piece (flagged for the gate)

- Two word header instead of the one word squeeze (see Blocks).
- Grow delivers new extents adopted as regions; contiguity is
  neither required nor exploited (see the grow contract).
- Extents are aligned up internally on adoption; the first debug
  round of the test caught a misaligned second extent and the
  lenient behavior was chosen over pushing alignment bookkeeping
  onto every host callback.
- `wh_realloc(p, 0)` frees and returns NULL; requests above half
  the address space are refused as overflow hygiene; realloc of an
  over-aligned pointer preserves contents but returns a
  base-aligned pointer. All three are documented in the header.

## How LithoLibC wraps this later

LithoLibC (piece 3 onward) creates one heap at startup over the
initial arena, installs the wasm grow shim and, once LithoThread
lands, a mutex behind the lock hooks, then calls
`wh_set_default_heap`. Its malloc family is then a rename:
`malloc` to `wh_libc_malloc`, `free` to `wh_libc_free`, `realloc`
to `wh_libc_realloc`, `calloc` to `wh_libc_calloc`, and
`aligned_alloc` / `posix_memalign` shaped calls onto
`wh_libc_aligned_alloc` with errno translation (NULL becomes ENOMEM)
at the libc boundary, where errno lives. Code that wants its own
arena (a level loader, a scratch space with a hard cap) skips the
default layer and uses an explicit `wh_heap_t` with no grow
callback: same allocator, explicit budget.

## Verification (host, this VM)

Commands run from `~/workspace/lk/lithoroot`:

    gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include -c src/waferheap.c -o /tmp/waferheap.o
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/waferheap_test.c src/waferheap.c -o /tmp/waferheap_test && /tmp/waferheap_test
    gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined -I include tests/waferheap_test.c src/waferheap.c -o /tmp/wh_asan && /tmp/wh_asan
    g++ -std=c++11 -Wall -Wextra -Werror -I include -c tests-style header check of include/waferheap.h

Results: strict C99 compile clean with no warnings; the test passes
all 12 cases (lifecycle, basic alignment and usable size, calloc,
aligned allocation at 32/64/256/4096, realloc grow and shrink,
realloc into a free successor, neighbor stomp check, 20,000-op
randomized churn against the shadow model ending in a single
coalesced free block, interleaved fragmentation then a 1 MiB
allocation, soft exhaustion on a fixed arena, grow adoption and
refusal, and the default heap layer); the AddressSanitizer and
UndefinedBehaviorSanitizer run passes clean; the header compiles
clean from C++11. A 32-bit host compile could not run on this VM
(no multilib headers installed); the 32-bit path differs only in
the pointer-width constants and one branch of the bit scans, and it
gets exercised for real when the wasm32 build adopts the library.

## Gate fix (Piece 3 review, Elfie, 2026-10-28)
wh_decode read the word before a payload BEFORE validating region
membership, so freeing a foreign pointer at the start of a mapped region
read below the mapping. Found by the Piece 3 builder exercising the
foreign-free path under ASan; fixed at the gate: wh_decode now checks the
pointer against every region's block range (payloads sit at least
WH_OVERHEAD inside) before any dereference. Verified by a guard-page
probe (PROT_NONE page below the arena: foreign frees at the arena base
are rejected cleanly, heap stays alive) and the full suite re-run, plain
and ASan+UBSan.
