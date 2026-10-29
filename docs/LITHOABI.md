# LithoAbi ... the C++ runtime glue (LithoRoot piece 6)

Status: landed for review, host-verified. LithoAbi provides, in-module,
every C++ ABI entry point the engine's compiled C++ needs once the vendor
runtime is gone: exception allocation and the throw / catch bookkeeping,
the personality decision procedure, guard variables for function-local
statics, the atexit family, the pure / deleted virtual stubs, RTTI and
dynamic_cast, and the std::exception family classes with their vtables
and typeinfos. It is wasm-first by design (the Windows build uses the
MSVC ABI and never links this piece).

Files:

- `include/cxxabi.h` ... the public surface (below), plus the seeded
  `include/exception`, `include/new`, `include/typeinfo` carrying the
  std::exception-family declarations the runtime anchors.
- `src/lithoabi/` ... `exception.cc` (allocation, emergency pool,
  refcount lifetime, eh globals, primary exceptions), `eh_runtime.cc`
  (throw / rethrow / begin / end catch, call_terminate /
  call_unexpected, the runtime's own bad_cast / bad_typeid /
  bad_alloc throws), `personality.cc` (the LSDA decision procedure),
  `typeinfo.cc` (type_info behavior, catch matching, the upcast and
  dynamic_cast walks), `guard.cc`, `atexit.cc`, `stubs.cc`,
  `std_exceptions.cc` (the std classes, handler variables, terminate
  paths). Internal headers: `lithoabi_internal.h` (exception header
  layout), `typeinfo_layout.h` (the __cxxabiv1 type_info classes),
  `lithoabi_atomic.h`, and `lithoabi_wasm_eh.h` (the target seam).
- `src/lithoabi/backend_wasm.cc` ... the wasm side of the seam
  (spellings unverified, see "Open risks"), and
  `src/lithoabi/backend_native_scaffold.cc` plus
  `lithoabi_test_scaffold.h`, the host-test scaffolding backend.
- `tests/lithoabi_test.cpp` ... the host test (17 cases, all passing;
  build recipes in its header).

## How the surface was derived

The census worked from the current binary (`wasm/full/LithoKraft.wasm`,
the Build 87c-era build the freeze note in `docs/LITHOSYS-FREEZE.md`
also measured), with a purpose-written Node parser (no wasm tools exist
on this VM):

- 142 imports total: 141 functions and 1 memory, all module "env".
  89 are ours (the `lk_*` surface). 53 are vendor.
- Exactly ONE import belongs to the C++ ABI family: `env.__cxa_throw`,
  signature (i32, i32, i32) -> ().
- The binary has no tag section and no tag import (the old toolchain
  split the runtime at the throw; the host's `lkboot.js` stub performs
  the non-returning act and documents the same caught-inside /
  uncaught-fatal contract LithoAbi keeps).
- The binary has NO name section and NO linking section, so the
  vendor libc++abi code statically linked inside it cannot be
  enumerated by name. Anyone claiming an exact defined-symbol census
  of the vendor runtime in this binary is guessing; the honest
  derivation is the one import above, plus binary content evidence,
  plus what the engine's source demonstrably uses.
- Content evidence (rodata strings): the typeinfo names
  `St9exception`, `St9bad_alloc`, `St20bad_array_new_length` are
  present, as are the `__cxxabiv1` class-type names
  (`__si_class_type_info`, `__vmi_class_type_info`, `__class_type_info`,
  `__pbase_type_info`, `__pointer_type_info`, `__function_type_info`,
  `__fundamental_type_info`, `__pointer_to_member_type_info`,
  `__shim_type_info`). `__cxa_thread_atexit` appears inside an assert
  string, proving it is defined and used in-module today. The "Pure
  virtual" diagnostic string is present. `St8bad_cast` and
  `St10bad_typeid` are absent (consistent with pointer-only
  dynamic_cast in the engine, which yields null instead of throwing).
- Engine source usage (grep over `engine/` and `game/`): 208 throw
  sites (46 of a custom RPC error type, the rest mostly
  std::runtime_error / std::invalid_argument / std::exception /
  std::bad_alloc / std::out_of_range), 47 catch (...) clauses, about
  20 catch (const std::exception&), std::exception_ptr /
  std::current_exception / std::rethrow_exception genuinely used
  across threads in the software renderer, 41 dynamic_cast / typeid
  sites, about 89 function-local statics, 3 nothrow-new sites.

The provided surface is therefore: the one imported function, the
families the compiler emits for the C++ the engine demonstrably uses,
and the principled minimum around them. `include/cxxabi.h` declares
all of it:

- Allocation: `__cxa_allocate_exception`, `__cxa_free_exception`,
  `__cxa_init_primary_exception`.
- Throw / catch: `__cxa_throw`, `__cxa_rethrow`, `__cxa_begin_catch`,
  `__cxa_end_catch`, `__cxa_get_exception_ptr`,
  `__cxa_current_exception_type`.
- Primary exceptions (the exception_ptr contract):
  `__cxa_get_globals`, `__cxa_get_globals_fast`,
  `__cxa_current_primary_exception`,
  `__cxa_rethrow_primary_exception`,
  `__cxa_increment_exception_refcount`,
  `__cxa_decrement_exception_refcount`.
- Terminate / unexpected: `__cxa_call_terminate`,
  `__clang_call_terminate` (the spelling clang emits in noexcept
  landing pads), `__cxa_call_unexpected`, `__cxa_bad_cast`,
  `__cxa_bad_typeid`, and the handler variables
  `__cxa_terminate_handler`, `__cxa_unexpected_handler`,
  `__cxa_new_handler`.
- Guards: `__cxa_guard_acquire`, `__cxa_guard_release`,
  `__cxa_guard_abort`.
- Exit: `__cxa_atexit`, `__cxa_finalize`, `__cxa_thread_atexit`,
  `__cxa_thread_atexit_impl`, `__dso_handle`.
- Stubs: `__cxa_pure_virtual`, `__cxa_deleted_virtual`.
- `__dynamic_cast`, and the personality entry `__gxx_personality_v0`
  (declared in the seam header, because its context parameter is the
  seam's opaque type).

Included but NOT imported by the current binary, with justifications:

- Everything except `__cxa_throw`: the vendor runtime provided these
  invisibly in-module; the toolchain piece compiles the engine's C++
  against our headers and will reference them directly. Each family
  maps to a demonstrated engine use (above).
- `__cxa_init_primary_exception`: completes the exception_ptr
  contract the renderer demonstrably uses; without it the stdlib
  piece cannot build a primary exception around an existing object.
- `__clang_call_terminate`: clang's landing pads name this entry,
  not `__cxa_call_terminate`; the toolchain piece uses clang.
- `__cxa_thread_atexit_impl`: the alternate spelling some targets
  emit for the same function; one line to provide, a missing symbol
  if absent.

Deliberately NOT provided:

- Dependent exceptions (`__cxa_allocate_dependent_exception` and
  family): only std::nested_exception uses them; nothing in the
  engine does. Additive later if that changes.
- The `__cxa_vec_*` array helpers: modern clang emits inline loops
  for array construction / destruction in exception paths. If the
  toolchain piece finds a reference, they are additive.
- `__cxa_demangle`: nothing references it; exceptions carry typeinfo
  pointers, not demangled strings. It may return with the stdlib
  piece if diagnostics want it.

Two symbols LithoAbi itself leaves undefined, by design and by the
same posture the reference runtimes take: `_ZnwmPv` (placement new;
declared in `eh_runtime.cc`, where the compiler inlines most uses)
and `_ZdlPvm` (sized operator delete; referenced by the deleting
destructors of the classes defined here). Both belong to the
operator new / delete family the stdlib piece owns.

## Exception handling design

The standing contract is unchanged: caught-inside works,
uncaught-fatal. `__cxa_throw` is now an in-module function; an
exception that no in-module handler matches ends the instance
through the terminate path rather than crossing the module boundary.

Exception header (`lithoabi_internal.h`): a refcounted layout.
Every allocation carries an atomic reference count starting at 1
(the in-flight ownership), then the Itanium `__cxa_exception`
fields in their fixed order (type, destructor, captured unexpected
/ terminate handlers, nextException, handlerCount,
handlerSwitchValue, actionRecord, languageSpecificData, catchTemp,
adjustedPtr), then the `_Unwind_Exception` (class value "GNUCC++\0"),
then two LithoAbi fields (flags, primaryObject) that exist for
`__cxa_init_primary_exception`, whose object lives in a separate
allocation. The thrown object always follows the whole header at
16-byte alignment, and every conversion goes through the header's
helpers, never open-coded arithmetic.

Lifetime is refcount-owned with a handler hold count beside it:
begin_catch takes a handler hold, end_catch releases the hold AND
the in-flight reference, rethrow hands the in-flight reference back
without touching it, `__cxa_current_primary_exception` adds a
reference the caller's exception_ptr owns, and
`lithoabi_maybe_release` is the single destroy-and-free point,
firing exactly when the count reaches 0 with no handler holding.
The test proves the cross-thread shape end to end: thrown and
caught on the main thread, kept alive past end_catch by the
primary reference alone, rethrown and caught on a weave thread,
destroyed exactly once at the far end_catch.

### The one-file seam

Every compiler-specific primitive lives behind
`src/lithoabi/lithoabi_wasm_eh.h`: the throw and rethrow delivery,
the caught-header recovery, the fatal-uncaught sink, and an opaque
personality context with five accessors (LSDA bytes, TType
resolution, install, unwind header, instruction pointer). All
portable logic is written against that interface only. Two shapes
are documented in the header: Shape A (expected: in-module wasm
exceptions, a C++ exception tag, the object pointer as the catch
payload) and Shape B (today's split at the throw, which cannot
deliver real in-module catches). `backend_wasm.cc` implements what
is already shape-independent (payload-to-header recovery, the
fatal sink) and routes the unreconciled primitives to a loud
terminate with a diagnostic naming the state, so an early ship
fails visibly instead of silently. `backend_native_scaffold.cc`
implements the same interface for the host test with a per-thread
in-flight slot and setjmp / longjmp catch frames; it models
delivery faithfully and is never in a production link.

### The personality procedure

`personality.cc` is a portable LSDA walker plus the two-phase
decision: call-site table search by instruction pointer, action
chain walking (catch entries matched through the thrown type's
`__do_catch`, catch-all, and exception-specification filters),
search phase versus cleanup phase, foreign exception class passes
through catches but still runs cleanups, and the handler-frame
install in phase 2. It is bounds-checked throughout and is
exercised by the test over hand-built synthetic LSDA bytes,
including a filter table. Two byte-level details are implemented
in their documented form and flagged for the toolchain piece to
confirm against real emitted tables: the action chain's
displacement is read as relative to the start of the displacement
field, and a filter's type list is read as uleb128 TType indices,
zero-terminated, from the action table start plus the negated
index. Dynamic exception specifications are removed from C++17
and unreachable from engine code, so the filter path is
completeness, not load-bearing behavior. TType entries are read
from the end of the TType table backwards (the header's TType
base is consumed as a byte offset from the LSDA start to the
table's end), and entries must use a fixed-size encoding.

## Emergency pool

`__cxa_allocate_exception` never returns NULL: LithoLibC malloc
first, then a static emergency pool, then the terminate path. The
pool is 8 slots of (header + 256 payload bytes), about 3.3 KB of
static storage, claimed through one atomic bitmap with no lock.
Slots 0..6 are general; slot 7 is reserved for the bad_alloc path
(`lithoabi_allocate_exception_for_bad_alloc`, consumed by
`lithoabi_throw_bad_alloc`, the designated entry the stdlib
piece's operator-new failure path will call). A general allocation
may borrow slot 7 only as its last resort before terminate; the
bad_alloc path takes only slot 7, so a heap that dies while
throwing bad_alloc still has somewhere to put it. 256 bytes of
payload covers every exception type the engine throws (the custom
RPC error and the std::runtime_error family are all well under
100 bytes). The test starves a tiny WaferHeap to exhaustion and
proves: 7 general allocations from the pool, bad_alloc served from
the reserved slot while all general slots are held, the reserved
slot borrowed by an 8th general allocation once returned, and (in
a child process) death by SIGABRT on the 9th.

## Guards

One global LithoThread mutex + condition variable serves all
guards (guard waits are rare and short; one lock keeps the state
machine auditable). The guard word's low byte is the state the
compiler's fast path reads (0 uninitialized, 1 initialized, 2
initializing); bits 32 and up carry the owning thread's LithoWeave
tid while initializing. A thread that finds its own tid in the
owner field is recursively initializing its own static: the
language leaves that undefined, and the implementation ends it
loudly through the terminate path rather than deadlocking (proven
by a death case). Abort by a non-owner is a documented no-op.
Release semantics ride the seq_cst atomic shim, so a reader that
observes state 1 has the happens-before edge from the
initializer's writes. The contention test runs 8 weave threads at
one guard and observes exactly one initializer.

## atexit wiring

`__cxa_atexit` keeps one newest-first list under a LithoThread
mutex. `__cxa_finalize` works in passes in LithoWeave's discipline:
detach the whole list, run the matching DSO's entries in reverse
registration order, defer non-matching entries order-preserved,
and repeat while registrations arrived during the pass, capped at
8 passes. `__cxa_thread_atexit` is a thin adapter over Piece 4's
`lw_thread_atexit` (the DSO handle is carried and ignored at
thread exit, matching reference behavior); the test observes
newest-first thread destructors at weave thread exit.
`__dso_handle` is defined weak: a hosted C runtime's crtbegin
provides the same token on native links and must win there, while
on wasm this definition is the only one; its value is its own
address either way.

One hosted-link note the test surfaced: in a native link,
LithoAbi's `__cxa_atexit` interposes the host libc's for the whole
process. That is the normal posture for a replacement C++ runtime
(the reference runtimes do the same), and registrations from the
host's own libraries carry foreign DSO handles and are simply
deferred by finalize. The wasm target has no host libc, so the
question does not arise there.

## RTTI and dynamic_cast

The `std::type_info` classes are declared in
`src/lithoabi/typeinfo_layout.h` in the ABI's layouts; the
compiler's emitted typeinfo objects and LithoAbi's walks share
that one definition of the shapes. Type identity is name-pointer
identity: within one statically linked module the compiler emits
exactly one name string per type, which is the comparison the
reference runtimes make as well. (Across module boundaries that
assumption would need the name-string comparison fallback; the
engine is one module.)

Catch matching implements the standard rules: class types through
the public-base upcast walk, pointers level by level with the
qualification rules in their multi-level form (qualifiers may be
added at a deeper level only under a const-carrying prefix),
class-pointee upcast at the outer level, void* pointee matching,
the null-pointer-value rule, and member pointers conservatively
(identical context class and member type; the full member-pointer
conversion lattice is the documented simplification, and engine
code does not throw member pointers). The fundamental typeinfo
objects are not spelled out in `typeinfo.cc`: the ABI's emission
rule anchors them to the translation unit defining
`__fundamental_type_info`'s key function, and the compiler emits
the whole set there as vague-linkage definitions; the test pass
verified all 21 (void through nullptr_t, including char8_t /
char16_t / char32_t) present in `typeinfo.o`.

Two encodings were verified empirically against host g++ emission
during the test pass, and one initial reading was wrong and is
corrected in the header with the evidence recorded:

- pbase qualifier bits: const 0x1, volatile 0x2, restrict 0x4,
  each pointer level describing its own pointee.
- vmi base offset flags: **virtual is 0x1, public is 0x2**, offset
  in bits 8 and up (negative, vtable-relative, for virtual bases).
  The first version of `typeinfo_layout.h` had public and virtual
  swapped; every public base read as non-public, so all
  multiple-inheritance catch adjustment and vmi dynamic_casts
  failed while single-inheritance (si) paths passed. The test
  caught it; believe the emission, not the memory.

`__dynamic_cast` follows the standard's clause order: the whole
object first, then the whole-object walk (a unique public
destination sub-object with the source sub-object publicly inside
the whole object: what makes sibling cross-casts work), then the
plain upcast within the source sub-object (a public base of the
static type answers even when that base is ambiguous in the
dynamic type; verified against the host runtime, whose ruling on
the diamond cases the test mirrors). Virtual bases are resolved
through the current sub-object's vtable, the reference
implementations' discipline.

## Tests

`tests/lithoabi_test.cpp` (recipes in its header, including the
house rename discipline: LithoThread's objects and LithoAbi's
pthread consumers are compiled with every pthread_* name mapped
to lt_pthread_*, so the native backends keep the host's pthread
symbols; without it, the weave's native spawn lands in
LithoThread's pthread_create and fails, which is how the
requirement was discovered here). 17 cases: allocation and header
layout, exact / base-adjusted / catch-all / non-matching catches,
rethrow, the runtime's own bad_alloc / bad_cast / bad_typeid
throws, the cross-thread primary handoff, init_primary and the
refcount family, guards basic and under 8-thread contention,
atexit ordering and per-DSO finalize and registration during
finalize, thread_atexit, the personality procedure over synthetic
LSDAs (catch, catch-all, cleanup, foreign class, handler frame,
filter select and non-select), dynamic_cast (downcast, cross-cast
in virtual and non-virtual diamonds, the ambiguity rulings,
upcast, null), terminate paths through a recording handler, the
emergency pool under a starved heap, and four death cases in
re-executed children (uncaught throw, recursive guard, pool
exhaustion, pure virtual), each asserted to die of SIGABRT.

Results, this pass: plain build ALL PASS. ASan + UBSan ALL PASS
(the C++ units take -fno-sanitize=vptr: UBSan's vptr check
recurses through its own cache-miss handler when the code under
test is the RTTI implementation). TSan: all cases pass
functionally, with exactly one warning, of the class Pieces 4
and 5 already diagnosed: two wh_calloc internal zeroing memsets
on the same recycled heap block, whose ownership transfer runs
through the uninstrumented allocator (compiled plain by the TSan
recipe), so the sanitizer cannot see the handoff; both writes
are by the block's owner at the time, ordered by the heap lock,
and no warning touches LithoAbi state.

What the scaffolding does NOT prove: the wasm throw itself. The
native backend models delivery (in-flight slot, longjmp to the
innermost frame); the landing pads in the test perform the
matching by hand. The tag, the throw intrinsic's spelling, the
compiler's landing-pad glue, and the real unwind are the
toolchain piece's to verify.

## Open risks for the toolchain piece

1. `backend_wasm.cc` spellings: the tag (candidate name
   "__cpp_exception"), the throw / rethrow intrinsics, and the
   personality context's concrete type behind the seam accessors.
   Until they land, the wasm backend's unreconciled primitives
   terminate loudly with a diagnostic.
2. The personality entry's name, linkage, and invocation shape in
   emitted code (in the DWARF model it is a C symbol named
   `__gxx_personality_v0`; the wasm model's shape is to be pinned
   down), and the two LSDA byte-level details flagged in
   `personality.cc` (action displacement base, filter list
   offset).
3. Confirmation that clang follows the same fundamental-typeinfo
   anchor rule on wasm32 (compiler emits the set in the key
   function TU), and the same pbase / vmi encodings probed here
   against g++ (the ABI fixes them; the probe was one compiler).
4. `_ZnwmPv` and `_ZdlPvm` arrive with the stdlib piece's
   operator new / delete family; a full module link waits on it.
5. Typeinfo identity is name-pointer identity within one module;
   if a second module ever links against this runtime, the
   equality rule needs its string-comparison fallback.
