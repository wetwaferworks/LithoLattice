# TOOLCHAIN: the stock-clang wasm32 build (Stage 2, final piece)

Status: LANDED and gate-verified, 2026-10-28. This piece replaces
the vendor compiler with stock clang targeting wasm32 plus the
LithoRoot sysroot, and proves the seven library pieces build and
run under it. It does NOT relink the game: game test builds are
frozen by JD's order, no engine or game source was touched, and
no CMakeLists was edited. The one new build file is
`cmake/lk-wasm32-toolchain.cmake` at the repo root.

## Toolchain provenance

* LLVM **20.1.8**, official release tarball
  `LLVM-20.1.8-Linux-X64.tar.xz`, from
  https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-Linux-X64.tar.xz
* Tarball size: **2,021,269,412 bytes**. SHA-256:
  **1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6**
  The digest matches the one GitHub's API publishes for the
  release asset (the release ships no separate .sha256 asset;
  fetching one returns 404). Re-verified on disk after install.
* Installed at `~/workspace/toolchain/LLVM-20.1.8-Linux-X64/`
  (nothing outside ~/workspace and /tmp). `clang --version` and
  `wasm-ld --version` both report 20.1.8; a hello-world wasm32
  object compiles.
* Download note, recorded so nobody re-diagnoses it: the first
  two attempts truncated (one silently at 1,935,699,968 bytes,
  one overshot on a bad resume seam). The accepted file was
  assembled from a fresh partial download plus three 256 MiB
  range chunks, each exact-size, and the assembled hash matching
  upstream is the proof the assembly is correct.
* Node on this VM: v24.22.1. It runs WebAssembly exceptions
  natively; every run proof below executed in it with no special
  flags.

## Sysroot layout and compile posture

The sysroot include root is `lithoroot/include` (the frozen
`lithosys.h` plus the pieces' own headers; clang's builtin
resource headers still supply compiler intrinsics such as
stdarg.h, which LithoRoot deliberately does not shadow).

Common flags for every library TU:

```
--target=wasm32 -matomics -mbulk-memory -msign-ext
-mthread-model posix -I lithoroot/include
```

* C pieces: `-std=c99 -O1 -Wall -Wextra`
* C++ pieces: `-std=c++17 -O1 -Wall -Wextra -fwasm-exceptions`
  (the standing EH contract: wasm exceptions, caught-inside /
  uncaught-fatal)
* Per-TU extras: `src/lithoweave.c` and `src/lithothread.c` add
  `-mllvm -wasm-enable-sjlj` (first-check (c)); `src/libc/setjmp.c`
  is compiled with `-fwasm-exceptions` (the SjLj throw builtin in
  it needs the EH mode; with only -mexception-handling, clang
  20.1.8's backend crashes on the weak longjmp tag).
* Link posture (probe, and the cmake file below): imported
  shared memory, `--import-memory --shared-memory`,
  initial 512 MiB / max 2 GiB in the cmake file (mirrors lkboot's
  `WebAssembly.Memory({initial: 8192, maximum: 32768, shared:
  true})`); the probe links smaller (16 MiB / 1 GiB) because the
  Node host supplies the memory.

## Per-piece compile results: 31 of 31 TUs compile for wasm32

Driver: `lithoroot/toolchain/build-libs.sh` (re-runnable; objects
and per-TU logs land in `lithoroot/toolchain/obj/`). All 21 C TUs
and all 10 C++ TUs compile clean under the flags above, including
`src/lithoabi/backend_wasm.cc`, the weave/thread wasm paths, and
the full LithoLibC (core, stdio, stdlib, math, time, scanf,
printf, setjmp).

Excluded by design (host test scaffolding and doubles, never part
of the wasm sysroot build): `src/libc/errno.c` (the LithoThread
link rule: a link containing LithoThread excludes it;
`lithothread_errno.c` provides `__litholibc_errno_location`),
`src/lithoweave_native.c`, `src/lithothread_native.c`,
`src/lithoabi/backend_native_scaffold.cc`,
`src/lithosys_volume_native.c`, and everything under `tests/`.

## First-check (a): the LithoThread wait seam... SETTLED, no change

The spellings Piece 5 isolated in `lithothread_wait.h` (and the
matching lines in `src/lithothread.c` and `src/lithoweave.c`),
`__builtin_wasm_memory_atomic_wait32((int*)addr, (int)expected,
timeout_ns)` and `__builtin_wasm_memory_atomic_notify((int*)addr,
(int)count)`, compile clean under stock clang 20.1.8 and lower to
real `memory.atomic.wait32` / `memory.atomic.notify`
instructions (objdump evidence; TU:
`lithoroot/toolchain/check_wait.c`). The header's isolation did
its job: nothing had to move.

## First-check (b): the LithoAbi wasm-EH seam... SETTLED, with three reconciliations

The emission model of stock clang 20.1.8 with `-fwasm-exceptions`,
decoded from probe objects and a linked probe module, and
cross-checked against upstream libunwind/libcxxabi sources
(studied, never copied):

* The throw primitive is `__builtin_wasm_throw(0, ptr)`: tag
  index 0 is the C++ exception tag `__cpp_exception`, and the
  payload is the `_Unwind_Exception` pointer (the unwind header
  inside LithoAbi's exception header), not the adjusted object
  pointer. `__cxa_begin_catch` already expects exactly that.
* Landing pads do selection themselves: a typed pad stores the
  LSDA address and its pad index into a thread-local context the
  RUNTIME must define, calls `_Unwind_CallPersonality(payload)`,
  then compares the context's `selector` field against the ttype
  index of its own catch clause. catch(...) pads and cleanup
  (catch_all) pads never consult the personality.
* The context is `_Unwind_LandingPadContext { uintptr_t
  lpad_index; uintptr_t lsda; uintptr_t selector; }`, defined in
  `backend_wasm.cc` as a thread_local named
  `__wasm_lpad_context`. `_Unwind_CallPersonality` resets the
  selector to 0 and calls the personality entry, whose wasm name
  is `__gxx_personality_wasm0`. LithoAbi's portable
  `__gxx_personality_v0` sits behind it through an adapter
  context in the back-end.
* The two LSDA byte details the Abi doc flagged, resolved:
  (1) the TType base value is a ULEB128 counted from the byte
  AFTER the encoded value (libcxxabi: `classInfo = lsda +
  classInfoOffset`, lsda already advanced), not from the LSDA
  start; (2) the call-site table holds one (landingPad, action)
  ULEB128 PAIR per personality-consulted pad, selected by 1-based
  pad index, not DWARF (start, length, lpad, action) ranges.
  Rather than teach personality.cc a second grammar, the wasm
  back-end TRANSLATES the envelope per call into the form the
  portable procedure reads (scratch buffer; the action table and
  TType table bytes are copied verbatim, so chains, indices, and
  typeinfo pointers are untouched). Details and the known
  limitation (a single try mixing typed clauses with a trailing
  catch(...), whose catch-all is a positive index with a null
  table entry that the walker reads as no-match) are written up
  in the header comment of `src/lithoabi/backend_wasm.cc`.

Three real defects fell out of this check and were fixed (see
"Source changes" for the full list):

1. **Destructor function type.** On wasm, destructors return
   `this`, so the destructor pointer the compiler hands
   `__cxa_throw` has type `void* (*)(void*)`, not
   `void (*)(void*)` (libc++abi's own note: "In Wasm, a
   destructor returns its argument"). LithoAbi stored and called
   it under the classic type, and the call_indirect in
   `__cxa_end_catch`'s release path trapped on the first
   polymorphic throw. The type is now keyed on `__wasm__` in one
   place (`__cxa_destructor_fn` in include/cxxabi.h). Verified:
   the vendor's own clang 24 emits the identical dest argument,
   so this is the wasm C++ ABI, not a stock-clang quirk.
   `__cxa_atexit` does not share the hazard: clang registers a
   compiler-synthesized `__cxx_global_array_dtor` wrapper with
   the classic signature there. Corollary for hand-written ABI
   callers: code that declares `__cxa_throw` itself with the
   classic dest type and passes a classic void destructor traps
   the same way; callers must use the shipped cxxabi.h (the
   piece's diag_abi driver was corrected to the wasm type for
   exactly this reason).
2. **Rethrow bookkeeping.** Compiler glue pairs every handler
   exit with a `__cxa_end_catch` (the handler body's cleanup pad
   runs it as an exception leaves). LithoAbi's `__cxa_rethrow`
   popped the caught chain eagerly, so that paired end_catch
   found an empty chain and terminated. The fix records a
   per-thread pairing token (`lithoabiRethrownPending`, a
   LithoAbi extension field in `__cxa_eh_globals`): rethrow
   unlinks and sets it, the paired end_catch consumes it and
   stands down, and `__cxa_begin_catch` clears it (so harnesses
   whose pads do not emit the paired call, like Piece 6's native
   scaffold, see the original semantics). The same token covers
   the head case of `__cxa_rethrow_primary_exception`.
   libc++abi's alternative (negating handlerCount) was tried
   first and convicted by the native gate: without the paired
   end_catch, the next begin_catch self-links the header. The
   token form passes BOTH the real compiler glue (this probe)
   and the unmodified native gate.
3. **Varargs advancement in printf/scanf** (found by the probe's
   diagnostic prints): see first-check (b) fallout under Source
   changes; it belongs to Piece 7's files but the wasm ABI is
   what convicts it.

Run proof (Node v24.22.1, wasm EH native): `lk_eh_probe()`
returns **31 of 31**... exact-payload catch across TUs, derived
caught as base with the adjusted pointer and vtable intact
(kind() == 2, base_field == 11, derived_field == 22 read back
through the object), rethrow caught by the outer handler,
catch(...) catching, and a local destructor running exactly once
during the unwind.

## First-check (c): the setjmp story... DECIDED, option (i), with our own runtime

Evidence (script: `lithoroot/toolchain/check_sjlj.sh`, TU
`check_sjlj.c`):

* With default flags, `setjmp`/`longjmp` are plain undefined
  calls: stock clang offers no in-module SjLj for wasm32 by
  default.
* With `-mllvm -wasm-enable-sjlj`, the only undefined symbols
  left are `__wasm_setjmp`, `__wasm_setjmp_test`, `__wasm_longjmp`
  (plus linker-provided table/stack symbols). Identical with
  `-fwasm-exceptions` added: in LLVM 20 the SjLj lowering does
  NOT switch to a tag-based longjmp; it uses wasm try/catch
  internally with an in-module tag.
* DECISION: adopt compiler SjLj (option (i)), but implement the
  three runtime functions OURSELVES in-module
  (`src/libc/setjmp.c`, header `include/setjmp.h`), so they are
  NOT host obligations and the frozen LithoSys surface does not
  grow. The semantics were pinned from the vendor compiler-rt
  source (read for semantics only) and cross-checked against the
  linked module's disassembly: `__wasm_setjmp(env, label,
  func_invocation_id)` stores the label and the invocation token
  (identity is pointer equality) into the jmp_buf;
  `__wasm_setjmp_test(env, id)` returns the stored label on a
  match, else 0; `__wasm_longjmp(env, val)` stores {env, val}
  into the args slot inside the jmp_buf (offset 8) and throws
  the in-module C_LONGJMP tag (tag index 1); value 0 resumes as
  1 per the standard. Our `jmp_buf` is 4 words (16 bytes).
* RUN PROOF: a probe module (the check TU plus our setjmp.c) has
  ZERO imports, and in Node `lk_sjlj_probe()` returned **71**:
  a longjmp of value 7 unwound through 3 recursive frames to the
  setjmp, and a second longjmp with value 0 resumed as 1.
* The native branch of `include/setjmp.h` defers to the host
  header (`#pragma GCC system_header` plus `#include_next`), so
  host builds are untouched. Native gates re-run after the
  header landed: LithoWeave **8 of 8 PASS**, LithoThread **13 of
  13 PASS** (see the recipe note in Findings).

## Probe link and import census (the headline proof)

Build, census, and run are two re-runnable commands, run from
the lithoroot directory: `bash toolchain/build-libs.sh` (the
31-TU matrix), then `bash toolchain/build-probe.sh` (probe
objects land in `toolchain/obj-probe/`).
The link is itself the gate: wasm-ld is given
`--allow-undefined-file=toolchain/lithosys-allowed.txt`, a file
listing exactly the 14 frozen `lithosys_*` functions. Any other
undefined symbol is a hard link error, not a silent import.

Probe content (`toolchain/probe_main.c` plus the EH TUs): heap
init over a static arena, weave main-thread init, TLS errno
arming, a malloc round trip, snprintf, a pthread create/join
round trip at link level (against a refusing host stub), C++
throw/catch across TUs, and a final printf through the frozen
fd_write import. The Node host (`toolchain/run_probe.mjs`)
supplies the shared memory and all 14 frozen calls as stubs;
`lithosys_thread_spawn` refuses with exactly `-LITHOSYS_EAGAIN`
(-6 in WASI numbering), and the probe requires pthread_create to
fail soft with EAGAIN.

Result: **failures = 0** (main returned 0).

Census of `toolchain/probe.wasm` (`node toolchain/census.mjs
toolchain/probe.wasm`), in full:

```
imports: 5
  env.memory : memory
  env.lithosys_fd_write : function
  env.lithosys_abort : function
  env.lithosys_console_write : function
  env.lithosys_thread_spawn : function
exports: 6
  __wasm_call_ctors : function
  __wasm_init_tls : function
  malloc : function
  lithosys_thread_start : function
  lk_probe_tls_block : function
  main : function
```

The linker GCs unreachable functions, so the probe's own census
shows only the 4 frozen calls its roots reach. The script
therefore links a second module with `--no-gc-sections`
(`toolchain/probe_full.wasm`) where every library function
survives, and censuses it too, in full:

```
imports: 11
  env.memory : memory
  env.lithosys_fd_write : function
  env.lithosys_volume_open : function
  env.lithosys_fd_close : function
  env.lithosys_fd_read : function
  env.lithosys_fd_seek : function
  env.lithosys_exit : function
  env.lithosys_abort : function
  env.lithosys_clock_now_ns : function
  env.lithosys_console_write : function
  env.lithosys_thread_spawn : function
```

10 of the 14 frozen functions are referenced by the libraries.
The 4 not referenced anywhere in library code are
`lithosys_memory_grow` (WaferHeap takes a grow callback; the
wiring that supplies this function as the callback is boot glue,
not library code), `lithosys_fd_sync`, `lithosys_random_fill`,
and `lithosys_cpu_count` (host-surface calls for engine/boot use;
LithoLibC's own rand is a self-contained generator by design).
In BOTH links the import set is a subset of the frozen surface
plus the imported memory: **no vendor symbol, no extra host
obligation, nothing else.** Memory posture: imported (the host
owns memory and growth, as the freeze intends).

## cmake/lk-wasm32-toolchain.cmake

A commented toolchain file at the repo root (93 lines): Generic
system identity for wasm32, `LK_LLVM_HOME` parameter (cache or
environment, defaulting to the installed LLVM tree above),
clang/clang++/llvm-ar/llvm-ranlib/wasm-ld paths, `CMAKE_SYSROOT`
pointed at lithoroot with find-root modes, the feature flags and
`-fwasm-exceptions`, try-compile as a static library (no host to
run test programs on), and the shared-memory linker posture
above. It was tested ONLY against a trivial throwaway cmake
project in /tmp (a C object plus a C++ object with a throw/catch,
configured and built into a static library): **cmake accepts it
and the build succeeds.** Testing it against the engine is the
relink's job; the freeze forbids it here.

The EMSCRIPTEN sites the future relink converts to
LL_TARGET_WEB (grep results, NOT edited):

* `engine/CMakeLists.txt:50` ... `if(NOT EMSCRIPTEN)`
* `engine/CMakeLists.txt:107` ... `if(NOT EMSCRIPTEN)`
* `engine/CMakeLists.txt:168` ... `if(NOT EMSCRIPTEN)`
* `engine/CMakeLists.txt:214` ... `if(NOT EMSCRIPTEN)`
* `engine/CMakeLists.txt:278` ... `if(LL_WITH_LITHOCEF AND NOT EMSCRIPTEN)`
* `engine/src/platform/CMakeLists.txt:7` ... `if(EMSCRIPTEN)`
* `game/LithoKraft/src/app/CMakeLists.txt:18` ... `if(NOT EMSCRIPTEN)`
  (a seventh site, in the game tree, beyond the six in the
  engine tree)

## lkboot patch plan (designs, not edits; lkboot.js untouched)

Facts from the current `wasm/full/lkboot.js` (read-only):
`lkFetchPack` (line ~4099) already fetches `LithoKraft.data` and
parses the LKP2 directory itself (magic, u32 header length, u16
directory count with per-directory lengths, u32 file count with
per-file directory index, name, LEB128 start delta and length,
all ranges validated), returning `{blob, files, dirs,
payloadStart}`. `lkBuildFS` (line ~306) builds a node tree over
that parse whose file nodes hold blob subarrays; fds live in a
Map with nextFd starting at 3, std fds preopened. `lkFdRead` /
`lkFdWrite` (lines ~474/498) already serve fd calls from that
table. The imports object is built by `lkBuildImports` (line
~3794); worker spawn today is vendor-shaped (`startThread` at
~4376, `_emscripten_thread_init`, `{cmd:'lk-done'}` recycle).

(a) **Thread spawn.** Add the page-side import
`lithosys_thread_spawn(start_arg)`: allocate a worker from the
existing pool and post a NEW message shape, say
`{cmd:'lk-litho-run', start_arg}`. The worker-side handler calls
the module export `lithosys_thread_start(start_arg)` and, when
it returns, performs the frozen tid-cell protocol BEFORE any
done/recycle message: `Atomics.store` the tid cell (at
start_arg's cell address, per the freeze) to 0, then
`Atomics.notify` on it. lkboot already has the i32 views and
uses Atomics.store/notify elsewhere (lines ~1007, ~1264,
~1301, ~1343), so this is assembly, not new capability. Do NOT
reuse the vendor init exports (`_emscripten_thread_init` and
friends): the weave owns TLS setup through
`lithosys_thread_start`. Each worker also needs its TLS block
initialized (`__wasm_init_tls`) before module code runs; the
weave's start path is the place to hang it.

(b) **Volume: Shape A (host parses the LKP2 directory)... 
recommended and, on the evidence, the only shape that fits.**
The page ALREADY parses the pack at fetch time and already
mints fds over the parsed node table. `lithosys_volume_open(path,
out_fd)` is then a small addition: look the path up in the
lkBuildFS tree; on a hit, allocate an fd over the existing node
(offset state per fd) and let the frozen fd_read/fd_seek/
fd_close serve it; on a miss return the frozen ENOENT so the
stdio layer's fallbacks engage. Shape B (the module registers
its own index) would need a registration channel that does not
exist in the frozen surface, and would duplicate a parse the
host already performs and validates. Cost of A: none to the
engine, none to the pack format.

(c) **Remaining frozen calls, from lkboot's near-equivalents.**
fd_write/fd_read/fd_close/fd_seek/fd_sync: the existing
lkFdRead/lkFdWrite bodies and fd Map are the substrate; the new
imports are thin adapters onto them, with the freeze's errno
mapping (ESPIPE for seek on the console fds, stdin as permanent
EOF). clock_now_ns: performance.now for MONOTONIC, Date.now
for REALTIME, both as u64 nanoseconds. console_write: the
existing console sink, cannot fail by contract. random_fill:
crypto.getRandomValues over the requested range. cpu_count:
navigator.hardwareConcurrency. exit/abort: frame-loop teardown
(abort prints loudly first; neither returns). memory_grow: the
host owns the WebAssembly.Memory object (created at line ~4240
as `{initial: 8192, maximum: 32768, shared: true}`); grow calls
memory.grow and refreshes the cached views, returning the
previous page count per the freeze.

## Source changes made by this piece (complete list)

New files:

* `lithoroot/include/setjmp.h`, `lithoroot/src/libc/setjmp.c`...
  the SjLj settlement (first-check (c)).
* `lithoroot/include/cstddef`, `include/cstdint`,
  `include/cstring`... thin wrappers (include the C header, lift
  the names into std::) that the sysroot lacked and the
  LithoAbi/new_delete sources include.
* `lithoroot/src/lithoabi/backend_wasm.cc`... REWRITTEN in place
  (this was the one anticipated seam reconciliation): the
  landing pad context definition, `_Unwind_CallPersonality`,
  `__gxx_personality_wasm0`, the adapter accessors, the LSDA
  envelope translation, and the throw/rethrow primitives. The
  native scaffold back-end is untouched.
* `lithoroot/toolchain/`... build-libs.sh, build-probe.sh,
  check_wait.c, check_sjlj.c/.sh, check_eh_throw.cpp,
  check_eh_catch.cpp, probe_main.c, census.mjs, run_probe.mjs,
  lithosys-allowed.txt, gate-lithoabi-native.sh, and three
  diagnostic drivers (diag_abi.cpp, diag_eh.cpp,
  diag_snprintf.c) kept as the evidence trail for the defects
  below, plus their objects and modules.
* `cmake/lk-wasm32-toolchain.cmake` (repo root).

Edits to pre-existing files:

* `lithoroot/include/new`: added `#include <cstddef>` and
  `enum class align_val_t : size_t {};` in std::, which
  new_delete.cc's aligned operators name. (The rest of Piece 7's
  <new> declaration debt stands as documented there.)
* `lithoroot/include/cxxabi.h`: the `__cxa_destructor_fn` alias
  (wasm: this-returning), applied to `__cxa_throw` and
  `__cxa_init_primary_exception`; the `lithoabiRethrownPending`
  extension field in `__cxa_eh_globals`.
* `lithoroot/src/lithoabi/lithoabi_internal.h`: the
  exceptionDestructor field now uses the alias.
* `lithoroot/src/lithoabi/eh_runtime.cc`: `__cxa_throw`'s dest
  parameter uses the alias; the runtime's own throws use a
  per-target destructor helper; `__cxa_rethrow` records the
  pairing token; `__cxa_end_catch` consumes it;
  `__cxa_begin_catch` clears it;
  `__cxa_rethrow_primary_exception`'s head case records it.
* `lithoroot/src/lithoabi/exception.cc`:
  `__cxa_init_primary_exception`'s destructor parameter uses the
  alias.
* `lithoroot/src/libc/printf.c` and `src/libc/scanf.c`: the
  va_list helpers (printf's fetch_signed/fetch_unsigned, scanf's
  store_signed/store_unsigned) now take the list BY ADDRESS,
  with the drivers (format_out, vscan_core) working on a local
  va_copy. Conviction: under wasm32 `printf("%d %d %d", 11, 22,
  33)` printed "11 11 11"... va_list is a scalar pointer type
  there, so a by-value helper copy advanced privately and every
  fetch re-read the first argument. The x86-64 native gate could
  not see it (array-type va_list shares state). The by-address
  form is the portable pattern and behaves identically on both.

Regression gates re-run for every touched piece: LithoWeave
native 8 of 8 and LithoThread native 13 of 13 after the setjmp
header; **LithoAbi native 17 of 17, zero failing checks**
(`toolchain/gate-lithoabi-native.sh`) after the EH changes;
Piece 7's stdio suite after the va_list fix: all printf/scanf
differential groups pass (24 of 25 groups; the one failure is
the pre-existing finding below, proven by control run).

## Findings (pre-existing, not caused by this piece)

1. **Piece 5 / Piece 7 test-recipe drift.** The LithoThread
   native test TU includes <time.h>, which since Piece 7
   resolves to LithoLibC's time.h, and that header deliberately
   does not declare clock_gettime/nanosleep. The documented
   recipe therefore fails at the TEST TU (the library TUs build
   clean). Control proof: the same recipe against a tree copy
   with this piece's setjmp files removed fails identically.
   Workaround used for the gate: a host-<time.h> shim include
   dir for the test TU only. The library itself is not involved;
   the pieces' docs should grow a note.
2. **Piece 7's stdio suite, audit group E-1** (exit() runs
   main-thread TLS destructors) fails in every arrangement
   reconstructible here on the WEXITSTATUS == 7 check, INCLUDING
   a control build with this piece's printf/scanf changes
   reverted, with identical output (24 of 25 groups pass). The
   standalone reproduction shows the forked child's weave state
   is the sensitive part (lw_thread_atexit returns an error in a
   bare child). This is an environment/recipe sensitivity in the
   suite's fork-child design, in code paths (stdlib exit, weave
   atexit) disjoint from everything this piece changed. Reported
   as a finding for the stdio piece's owners; not silently
   fixed.

## Honest remaining risks

* **Only a real game relink proves the whole.** The probe links
  the libraries against each other and a stub host. The engine's
  own code (hundreds of throw sites, 41 dynamic_cast/typeid
  sites, function-local statics through LithoAbi guards, real
  LithoThread workloads) will exercise paths the probe cannot:
  multi-record action chains with cleanups interleaved, filter
  (exception specification) records, LSDA translation for very
  large functions (the translator caps at 256 call-site pairs
  per LSDA and declines the pad loudly beyond it... a real cap
  to lift if the engine hits it), and the volume host under a
  real pack.
* **The mixed typed + catch(...) limitation** (first-check (b)):
  a single try block mixing typed clauses with a trailing
  catch(...) can miss its catch-all under the current walker.
  Standalone catch(...) works. The fix belongs to a LithoAbi
  follow-up in personality.cc's walk_chain.
* **The host side does not exist yet.** lkboot implements none
  of the 14 imports today; the patch plan above is a design,
  and thread spawn in particular (worker protocol, TLS init,
  the tid-cell store-then-notify order) can only be proven with
  the page and a real worker pool.
* **Device proof is still owed.** Everything here ran on the
  build VM (Node). The Motorola's WebView (Chromium 155) is a
  newer engine than Node's in some respects and older in none
  that matter for wasm EH, but the standing rule holds: the
  phone is the verdict, and it has not seen this build.
* **The toolchain file's linker posture for the GAME link** (as
  opposed to the probe's) is untested: symbol export lists,
  entry/ctor arrangements, and the .data delivery are relink
  decisions this piece deliberately did not make.
