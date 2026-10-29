# LithoSys Freeze Note ... version 1

Status: PIECE 1 of the LithoRoot program, landed for Elfie's per-piece
gate. The frozen header is `include/lithosys.h`; the compile checks are
`tests/lithosys_check.c` and `tests/lithosys_check.cpp`. Nothing in the
existing engine or game source was touched, and no other LithoRoot
piece was started.

The freeze in one line: when the vendor toolchain and its precompiled
runtime are gone, the wasm build will ask its host for exactly
**13 functions**, all declared in `lithosys.h`, all imported under
module "env", and nothing else beyond the platform surface that is
already ours (the `lk_*` services and the shared linear memory).

The dossier estimated 15 to 20. The freeze lands at 13 because the
census below proved several candidates unnecessary: atomic wait and
notify are wasm instructions, not imports; the current memory size is
the memory.size instruction; the vendor's fd-interrogation and
environment calls exist only to serve a generic libc, and LithoLibC
owns its own fd table and environment; and the vendor's main-thread
mailbox family disappears under the frozen any-thread contract. A
smaller frozen surface is the point of the exercise.

## (a) Import census of the current binary

Binary: `wasm/full/LithoKraft.wasm`, 6,439,394 bytes, SHA-256
`a4e4f19e1d0878816fb29b9a8fd679ba2930ea6e6e182cd82f2eabc9484bbd27`
(the staged build current at freeze time; test builds are suspended,
so this binary is a census source, not a deliverable).

Method: no wabt tools exist on this VM, so the census was taken two
independent ways that agree exactly: Node's
`WebAssembly.Module.imports()` for module, name and kind of every
import, and a purpose-written parser of the wasm type and import
sections for exact signatures.

**Total: 142 imports** (141 functions + 1 memory), all under module
"env".

| Class | Count | Notes |
| --- | --- | --- |
| Already ours: `lk_gl*` GL entry points | 69 | from lk_gl_imports.h, provided by lkboot |
| Already ours: other `lk_*` platform services | 18 | the lk_web.h surface plus lk_mega_draw and lk_mega_probe |
| Already ours: `lkio_present` | 1 | software presenter, declared in software_presenter_webgl.cpp |
| Already ours: `memory` | 1 | the shared linear memory itself; the host creates it in every design |
| **Vendor: the removal targets** | **53** | all function imports, listed below |

The 53 vendor imports, grouped by family, with exact signatures
(i32 unless noted; pointers are wasm32 linear-memory addresses):

WASI-named descriptor, clock, random and environment calls (10):

| Import | Signature |
| --- | --- |
| clock_time_get | (i32, i64, i32) -> i32 |
| environ_get | (i32, i32) -> i32 |
| environ_sizes_get | (i32, i32) -> i32 |
| fd_close | (i32) -> i32 |
| fd_fdstat_get | (i32, i32) -> i32 |
| fd_read | (i32, i32, i32, i32) -> i32 |
| fd_seek | (i32, i64, i32, i32) -> i32 |
| fd_sync | (i32) -> i32 |
| fd_write | (i32, i32, i32, i32) -> i32 |
| random_get | (i32, i32) -> i32 |

The vendor libc's private `__syscall_*` plumbing (23): chmod (i32,
i32), dup (i32), dup3 (i32, i32, i32), fchmod (i32, i32), fcntl64
(i32, i32, i32), fstat64 (i32, i32), ftruncate64 (i32, i64), getcwd
(i32, i32), getdents64 (i32, i32, i32), ioctl (i32, i32, i32),
lstat64 (i32, i32), mkdirat (i32, i32, i32), newfstatat (i32, i32,
i32, i32), openat (i32, i32, i32, i32), poll (i32, i32, i32),
poll_nonblocking (i32, i32), readlinkat (i32, i32, i32, i32),
renameat (i32, i32, i32, i32), rmdir (i32), stat64 (i32, i32),
symlinkat (i32, i32, i32), truncate64 (i32, i64), unlinkat (i32, i32,
i32). All return i32.

Vendor runtime services (9):

| Import | Signature |
| --- | --- |
| _abort_js | () -> () |
| emscripten_check_blocking_allowed | () -> () |
| emscripten_date_now | () -> f64 |
| emscripten_exit_with_live_runtime | () -> () |
| emscripten_get_heap_max | () -> i32 |
| emscripten_get_now | () -> f64 |
| emscripten_num_logical_cores | () -> i32 |
| emscripten_resize_heap | (i32) -> i32 |
| exit | (i32) -> () |

Vendor thread JS helpers (7):

| Import | Signature |
| --- | --- |
| __pthread_create_js | (i32, i32, i32, i32) -> i32 |
| _emscripten_init_main_thread_js | (i32) -> () |
| _emscripten_notify_mailbox_postmessage | (i32, i32) -> () |
| _emscripten_receive_on_main_thread_js | (i32 x7) -> f64 |
| _emscripten_thread_cleanup | (i32) -> () |
| _emscripten_thread_mailbox_await | (i32) -> () |
| _emscripten_thread_set_strongref | (i32) -> () |

Vendor time and locale JS helpers (2): _localtime_js (i64, i32) ->
i32; _tzset_js (i32, i32, i32, i32) -> ().

ABI and assert imports (2): __assert_fail (i32, i32, i32, i32) -> ();
__cxa_throw (i32, i32, i32) -> ().

For the record: today's page layer (lkboot.js) carries an entry for
nearly every one of these 53 names, a mix of real shims, fail-soft
stubs and loud throwers left over from the purge cleanup. The freeze
replaces that whole vendor-named surface with our own names and our
own semantics.

## (b) Mapping: every vendor import to its LithoSys fate

Replaced by a frozen LithoSys function:

| Vendor import | LithoSys replacement |
| --- | --- |
| emscripten_resize_heap | lithosys_memory_grow |
| clock_time_get, emscripten_get_now | lithosys_clock_now_ns (MONOTONIC) |
| emscripten_date_now | lithosys_clock_now_ns (REALTIME) |
| __pthread_create_js | lithosys_thread_spawn |
| fd_write | lithosys_fd_write |
| fd_read | lithosys_fd_read |
| fd_close | lithosys_fd_close |
| fd_seek | lithosys_fd_seek |
| fd_sync | lithosys_fd_sync |
| random_get | lithosys_random_fill |
| emscripten_num_logical_cores | lithosys_cpu_count |
| exit, emscripten_exit_with_live_runtime | lithosys_exit |
| _abort_js | lithosys_abort |

Eliminated as imports, with the reason for each:

- emscripten_get_heap_max: the heap cap becomes a policy of
  lithosys_memory_grow, expressed as an ENOMEM refusal. WaferHeap
  needs the refusal, not the number.
- emscripten_check_blocking_allowed: it guarded vendor operations
  that could block the main thread. The frozen surface contains no
  indefinitely blocking call, so there is nothing to guard.
- fd_fdstat_get: it let a generic libc interrogate fds it did not
  create. Every fd in the frozen design is created by the surface
  itself, and LithoLibC tracks each fd's class in its own table.
- environ_sizes_get, environ_get: the environment becomes an
  in-module table owned by LithoLibC, empty on the web host, so
  getenv answers NULL without any host call. See open question 3.
- _emscripten_thread_cleanup: thread-local and runtime cleanup run
  in-module under LithoThread before the start function returns. The
  host's only exit duty is the frozen tid-cell store plus notify in
  the spawn contract.
- _emscripten_thread_set_strongref: it pinned the JS Worker object
  against garbage collection, a vendor runtime concern. Our page
  layer owns its worker registry explicitly.
- _emscripten_init_main_thread_js: main-thread thread-local setup is
  in-module initialization under LithoWeave and LithoThread.
- _emscripten_thread_mailbox_await,
  _emscripten_receive_on_main_thread_js,
  _emscripten_notify_mailbox_postmessage: the mailbox family proxied
  main-thread services to worker threads. The frozen contract makes
  every LithoSys function callable from any thread, so no proxy
  exists. Inter-thread wakeups use the atomic notify instruction.
- _tzset_js: timezone rules are data, not a host service. See open
  question 2.
- _localtime_js: civil-time conversion is pure arithmetic on the
  realtime clock and belongs in-module in LithoLibC.
- __assert_fail: becomes a function LithoLibC defines itself. It
  reports through lithosys_console_write and ends in
  lithosys_abort. It was never truly a host service.
- __cxa_throw: becomes a symbol provided in-module by LithoAbi
  (piece 7 of the program). Listed here so the census closes; it is
  not a LithoSys matter.
- The 23 `__syscall_*` calls, as a family: they are the vendor
  libc's private plumbing for path-based POSIX file I/O (openat,
  stat64, newfstatat, lstat64, fstat64, mkdirat, renameat, unlinkat,
  rmdir, symlinkat, readlinkat, truncate64, ftruncate64, chmod,
  fchmod, getdents64, getcwd), for descriptor plumbing (dup, dup3,
  fcntl64, ioctl) and for readiness polling (poll,
  poll_nonblocking). LithoSys freezes descriptor I/O only. The path
  family is open question 1; the plumbing has no consumer once
  LithoLibC owns its fd table; and poll exists to serve sockets and
  files, which the fail-soft contract below covers without it.

## (c) Host capabilities the vendor runtime provides implicitly

The census counts imports, but the vendor runtime also supplies
capabilities that never appear as imports. Each is accounted for:

- **Heap growth, cap, and view refresh.** Covered by
  lithosys_memory_grow: the host grows the shared memory, enforces
  its maximum, and refreshes its cached views before returning. The
  frozen rule that in-module code never uses the memory.grow
  instruction directly is what keeps the host's views coherent.
- **Current memory size.** Not a capability gap: the memory.size
  instruction answers it in-module.
- **Clocks.** Both clocks the engine uses (a monotonic timebase and
  wall time for timestamps) are covered by lithosys_clock_now_ns.
- **Thread spawn and exit signalling.** Covered by
  lithosys_thread_spawn plus the frozen exit protocol: the host
  zeroes the tid cell and notifies its address when a worker's start
  function returns, which is all LithoThread's join needs.
- **Thread cleanup.** In-module by design (see the mapping for
  _emscripten_thread_cleanup). No host capability required.
- **Logging and stdio output.** Covered twice on purpose:
  lithosys_fd_write for fd 1 and 2 in normal operation, and
  lithosys_console_write as the state-free sink for boot, assert and
  abort paths.
- **Standard input.** Frozen as permanent end-of-file on hosts with
  no input stream, so stdio reads behave ordinarily.
- **Randomness.** Covered by lithosys_random_fill.
- **Exit and abort.** Covered by lithosys_exit and lithosys_abort,
  with teardown (including stopping the frame loop) owned by the
  host.
- **Program arguments.** The vendor's JS glue synthesizes argv
  before main; no import carries it. In the LithoRoot design argv is
  built by our own startup code in-module. The web host passes no
  arguments today, and the engine reads its URL parameters through
  the lk platform layer, not through argv, so nothing is lost.
- **Environment variables.** An in-module table in LithoLibC, empty
  on web. See open question 3.
- **Local time.** Civil-time conversion moves in-module over the
  realtime clock. Version 1 of the freeze works in UTC; see open
  question 2.
- **The virtual filesystem behind the data pack.** NOT covered by
  this freeze, deliberately. Today the vendor preloads
  LithoKraft.data into its in-memory filesystem and engine code opens
  pack files by path through ordinary stdio. That is a real,
  load-bearing capability and replacing it is a design decision, not
  a detail. It is open question 1, and it blocks the LithoLibC stdio
  piece until answered.
- **Main-thread service proxying.** Eliminated by the any-thread
  contract rather than replaced.

## (d) The fail-soft contract

On the web host today, the engine's network listener (the MCP
listener) asks the C library for sockets, and those calls fail soft
inside the vendor libc: ordinary error returns, the listener stands
down, and the engine runs on. The freeze preserves that behavior as
an explicit contract, in five rules:

1. **Unsupported means an error code, never a trap.** Every fallible
   LithoSys function returns a lithosys_err_t. A host that cannot do
   something answers LITHOSYS_ENOSYS (or a more specific code). No
   LithoSys call aborts the module for being unsupported.
2. **Every frozen symbol must exist.** A host that cannot implement
   a function still provides it, returning ENOSYS. A missing symbol
   fails instantiation loudly at load, which is the behavior the
   Stage 1 purge established on purpose: loud at load beats silent
   at first use. An ENOSYS answer is checkable and loggable; it is
   not the pre-purge silent zero stub.
3. **LithoLibC translates, callers keep their shape.** LithoLibC
   turns LithoSys codes into the usual C failure shapes (minus one
   and an errno, or EOF), so existing call sites written against
   ordinary libc failure handling work unchanged.
4. **Sockets fail locally, without any host call.** Sockets are not
   in the LithoSys surface at all. LithoLibC implements its socket
   entry points itself: they set an ENOSYS-flavored errno and return
   failure. The MCP listener sees exactly the ordinary failure it
   sees today, concludes it cannot listen, and the engine continues
   without it. The same local-failure shape applies to path-based
   file opens until open question 1 is answered.
5. **The console always works.** Fds 1 and 2 and
   lithosys_console_write succeed on any host that can show text at
   all, so logging, diagnostics and assert reports are never the
   thing that fails. The small fail-soft choices are frozen with the
   surface: stdin reads return EOF, closing a reserved fd is a no-op
   success, and seek on the web host's fds answers ESPIPE.

## (e) Open questions, flagged not decided

1. **Path-based file access and the data pack (blocking).** How does
   the engine read pack files by path once the vendor filesystem is
   gone? A candidate shape exists: a small read-only volume surface
   (the host registers the fetched pack blob; an open-by-path call
   returns an fd that the frozen fd_read, fd_seek and fd_close then
   serve). It is sketched here only as a candidate: it touches the
   pack format and possibly the workstation tree, so it needs a
   ruling from Elfie and JD before the LithoLibC stdio piece is
   designed.
2. **Local timezone.** The freeze assumes LithoLibC civil time is
   UTC. If JD wants device-local timestamps in logs, the addition
   would be a single offset query function. Not frozen; the question
   is whether it is wanted at all.
3. **Environment variables.** Frozen as an empty in-module table. If
   page-provided pairs (for example derived from URL parameters) are
   ever wanted through getenv, that is an addition to the surface,
   not a change to it.
4. **Spawn exit protocol, a verification rather than a decision.**
   The frozen protocol requires the web host to store 0 into the tid
   cell and Atomics.notify its address when a worker's start
   function returns. This is the standard wasi-threads shape, but
   the LithoWeave piece should verify it against the real lkboot
   worker wrapper before LithoThread builds join on top of it.

## (f) The frozen surface, for the gate

Thirteen functions, all in `include/lithosys.h`:

| Function | Purpose |
| --- | --- |
| lithosys_memory_grow | grow shared memory through the host, cap enforced |
| lithosys_clock_now_ns | monotonic and realtime clocks, nanoseconds |
| lithosys_thread_spawn | spawn a worker thread (LithoWeave contract) |
| lithosys_fd_write | gather-write to an fd; fds 1 and 2 are the console |
| lithosys_fd_read | scatter-read from an fd; stdin is permanent EOF |
| lithosys_fd_close | close an fd; reserved fds are a no-op success |
| lithosys_fd_seek | reposition a seekable fd; ESPIPE on web |
| lithosys_fd_sync | flush an fd; ENOSYS where no durable files exist |
| lithosys_console_write | state-free last-resort log sink, cannot fail |
| lithosys_random_fill | host entropy |
| lithosys_cpu_count | logical core count, 1 when unknown |
| lithosys_exit | end the instance with a code, never returns |
| lithosys_abort | abort the instance now, never returns |

## (g) Host compile check

The header is a contract, so it must compile clean as strict
portable C99 and from C++. The checks take the address of every
declared function into a variable of its exact declared type, which
fails on any signature drift or missing declaration. Compile only:
the definitions belong to the host, so nothing is linked.

Commands (run from `~/workspace/lk/lithoroot`):

    gcc -std=c99 -Wall -Wextra -Werror -I include -c tests/lithosys_check.c -o /tmp/lithosys_check_c.o
    g++ -std=c++11 -Wall -Wextra -Werror -I include -c tests/lithosys_check.cpp -o /tmp/lithosys_check_cpp.o
    gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include -c tests/lithosys_check.c -o /tmp/lithosys_check_ped.o

Results: all three compile clean, no warnings (the third command is
a bonus strictness pass beyond the required two).

## Ruling addendum (JD, 2026-10-28 17:48 EDT, via Elfie)
Open question 1 is CLOSED. JD adopted the read-only volume: the host
registers the fetched pack blob as a read-only volume; one new call
(open-by-path within the volume, exact name and signature to be set by
the LithoLibC stdio piece under this ruling) returns an fd that the
frozen fd_read, fd_seek and fd_close serve. The pack format is
unchanged; engine stdio call sites are unchanged in shape. Frozen
surface: 13 calls plus this one volume open, 14 total. The raw-blob
single-fd alternative was considered and rejected (blast radius into
engine and workstation code).

## Host minting note (audit fix run, 2026-10-28)
The volume open returns an fd that the frozen fd calls then
serve, so the minting end carries obligations the signature
cannot express. A volume host mints fds only in its own
namespace, at 3 or above: descriptors 0, 1 and 2 belong to the
process, and a minted fd that collides with one of them makes
the volume's bytes somebody else's stream. A volume serves
regular pack files; an open that resolves to anything else (a
directory, a device, a fifo) is a miss, not an fd. Symlink
policy is a host obligation: whether a name inside the volume
may resolve outside it is decided by the host that registered
the volume (the lkboot integration list in
docs/LITHOLIBC-STDIO.md carries the same note), never by the
caller. The native host double in src/ was brought under the
first two rules by the 2026-10-28 fix run (an fstat shape
check plus descriptor numbering at mint time); it follows
symlinks, as a plain directory server does, which is exactly
why the policy has to be stated for the real hosts.
