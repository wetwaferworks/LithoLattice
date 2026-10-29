# LithoLibC stdio / stdlib / math / time, and the new/delete family

Seventh piece of the LithoRoot program, and the last library piece
before the toolchain swap. It lands:

* `include/stdio.h`, `include/stdlib.h`, `include/math.h`,
  `include/time.h`: our own C99 headers, standard shapes, extern "C"
  guards, declaring exactly the derived surface below.
* `src/libc/stdio.c`, `printf.c`, `scanf.c`: a real FILE layer and
  the format families, over the frozen LithoSys fd calls.
* `src/libc/stdlib.c`: conversions, sort/search, process control.
* `src/libc/math.c`: the census libm, portable implementations.
* `src/libc/time.c`: civil time in UTC, per the freeze.
* `src/libc/strtod.c`, `dtoa.c`, `bigint.c`: the exact decimal
  conversion machinery both directions share.
* `src/libc/new_delete.cc`: the operator new/delete family,
  completing the debt LithoAbi left (docs/LITHOABI.md).
* `include/lithosys.h` gains ONE declaration,
  `lithosys_volume_open`, under the 2026-10-28 volume ruling
  addendum to docs/LITHOSYS-FREEZE.md. It is the only pre-existing
  file this piece touches, and only by that addition. The frozen
  surface is now 14 calls.
* `src/lithosys_volume_native.c` and `tests/lithosys_host_double.c`:
  host test doubles, scaffolding in the pattern of the weave and
  thread native back-ends.
* `tests/libc_stdio_test.c` and `tests/libc_new_delete_test.cpp`:
  the host gates.

## How the surface was derived

Census first, code second. The surface is the set of functions the
engine and game actually call, counted by grep over
`~/workspace/lk/engine` and `~/workspace/lk/game` (pattern: word
boundary, name, open paren; counts are call-site lines, gathered
2026-10-28), plus the completion set a C++ runtime expects around
them. Anything not called and not needed for compilation is NOT
declared: an undeclared function fails a build loudly, while a
declared-but-stubbed one fails silently at run time, and the freeze
prefers the loud failure. The counts that sized the piece:

stdio: printf 514, fprintf 103, snprintf 140, vsnprintf 2, fopen 34,
fclose 41, fread 16, fwrite 11, fseek 8, ftell 4, rewind 8, fflush 16,
fgets 1, fputs 2, ferror 3, sscanf 10, fscanf 1 (its one real format
is "%lu %lu", process_memory.cpp). feof: 0 direct calls, declared
because the FILE layer's own idiom and the completion set need it.

stdlib: atoi 23, atol 3, strtol 12, strtoll 3, strtoul 4, strtoull 1,
strtod 10, strtof 3, atof 8, abs 150 (the wider sweep including
std::abs forms counted 226), qsort 2, getenv 98 (the LL_* flag
family; the wider sweep counted about 101), exit 6, abort 14,
atexit 1. system: 21 raw grep hits, every one a false positive
("system" inside other identifiers and prose); the real count is 0,
and system() is declared only because stdlib.h's standard shape
carries it, failing soft (below).

math: floor 220 (298 with std:: forms), fabs 115 (140), isfinite 82
(117), sqrt 112 (114), cos 72 (87 with std:: forms), sin 78 (86),
log 83 (84), ceil 63, pow 22 (23), atan2 15, sqrtf 11 (13), hypot 11,
exp 8, round 8 (9), log2 7, fmod 7, acos 7, tan 6, asin 6, fmin 5,
trunc 2, fmax 1. modf, frexp, ldexp: 0 call sites; implemented
anyway because the math internals themselves are built on them and
the completion set expects them.

time: time_t appears at 3 real sites, localtime_r at 3
(engine/src/core/log.cpp, fragment_gallery.cpp, screenshot.cpp),
time( as a call 38 lines including declarations and unrelated
receivers. clock_gettime, nanosleep, mktime, gmtime, strftime: 0
call sites. mktime, gmtime and strftime are implemented as the sane
completion set (a log timestamp path wants them the day it is
written); clock_gettime and nanosleep are NOT declared, because
LithoThread already rides lithosys_clock_now_ns directly and no
caller wants the POSIX shapes.

The implemented surface, counted from the linked objects: 216
public and cross-file symbols across this piece and the core it
builds on (the ASan rename list below enumerates them mechanically).

## The volume, and how fopen routes

JD's ruling (2026-10-28 17:48): the host registers the fetched pack
blob as a read-only volume; one new LithoSys call opens a file by
path in it. The declaration added to `include/lithosys.h`, in the
frozen style:

```c
lithosys_err_t lithosys_volume_open(const char* path,
                                    uint32_t* out_fd);
```

One refinement from the suggested shape: the out parameter is
`uint32_t*`, not `int32_t*`, because every other out parameter in
the frozen header is unsigned, and fd values in LithoSys are
uint32_t throughout. On success it returns LITHOSYS_OK and an fd
that the frozen fd_read / fd_seek / fd_close serve; volume fds ARE
seekable in-host despite the freeze's general web note about
ESPIPE. A path the volume does not hold returns LITHOSYS_ENOENT;
a host with no volume registered returns LITHOSYS_ENOSYS. It never
traps.

fopen's routing, in order:

1. The path is offered to the volume first, whatever the mode.
2. Volume hit, read mode (r, rb, and read-dominant forms
   without +): the stream opens on the volume fd.
3. Volume hit, any write intent (w, a, x, or + with write): the fd
   is closed and fopen fails with EROFS. The volume is read-only;
   pretending otherwise would corrupt the caller's expectations.
4. Volume miss (ENOENT from the call): fopen fails with ENOENT.
5. No volume registered (ENOSYS from the call): a read open fails
   with ENOSYS; a write open also fails with ENOSYS, because the
   frozen surface has no writable filesystem at all. Persistence in
   the engine rides the lk platform layer, not stdio; that boundary
   is deliberate and is stated here so nobody routes a save file
   through fopen by accident.

Per-function fail-soft table (everything fails soft with errno,
nothing traps, per the freeze):

| Function | Volume / frozen-surface behavior |
|---|---|
| fopen read, volume hit | opens, seekable |
| fopen write intent, volume hit | NULL, EROFS |
| fopen read, volume miss | NULL, ENOENT |
| fopen any mode, no volume | NULL, ENOSYS |
| fread / fgetc / fgets on volume fd | served by fd_read |
| fseek / ftell on volume fd | served by fd_seek |
| fwrite / fputc on a read stream | error flag, EBADF-class errno |
| remove, rename | -1, ENOSYS (no writable namespace exists) |
| tmpfile, tmpnam | not declared (no writable scratch exists) |
| system() | -1, ENOSYS |
| socket-adjacent surface | none declared; nothing to fail |

stdin, stdout, stderr are preopened FILEs on fds 0, 1, 2. stdout is
line-buffered when the sink behaves like a console and fully
buffered otherwise, in the usual shapes; stderr is unbuffered.

## stdio layering over LithoSys

Two sinks exist under stdio, and the layering is a rule, not a
preference:

* The normal path is fd_write (and fd_read). Every byte of ordinary
  program output, including printf to stdout and stderr, travels
  through the frozen fd calls, so buffering, interleaving and error
  reporting behave like a file layer should. One global LithoThread
  mutex guards the stream table and every stream's state; the std
  streams carry static 1024-byte buffers, fopen'd streams a 4096
  byte malloc'd buffer.
* lithosys_console_write is the sink of last resort and stdio
  proper NEVER uses it. It belongs to the reporters that must speak
  when the fd layer itself may be the thing that just failed: the
  assert and abort reporters and LithoAbi's diagnostics. Reserving
  it keeps a dead fd layer from taking the last error message down
  with it. (The freeze comment on the call says the same; this
  piece's stdio honors it by construction.)

## The printf float contract

Float conversion is exact. dtoa (src/libc/dtoa.c) expands the true
binary value into its full decimal digit string through the stack
bigint (src/libc/bigint.c, fixed capacity, no allocation), and the
formatter rounds that exact expansion at the requested cut with
round-half-even. Consequences, all intentional:

* %.17g round-trips every double; %g chooses its style from the
  ROUNDED exponent, as the standard requires; trailing zeros trim
  in %g unless # is given.
* The engine's real formats are covered: %.17g, %.9g, %zu (182
  uses), %llu (174), %lld, and the scanf shapes "%d,%d", "%d,%d,%d",
  "%f,%f,%f", "%lu %lu".
* No long double anywhere: the l length modifier on a float
  conversion is accepted and ignored (it means double), and L is
  treated as l. The freeze leaves long double out of the surface.
* %a is NOT implemented: the census found 0 call sites. An %a (or
  any unknown conversion) prints literally and consumes no
  argument, which is visible in output rather than silently wrong.
* %s of NULL prints "(null)" and %p of NULL prints "(nil)", the
  host behaviors the differential gate compares against. %n is
  supported.

scanf is the mirror: integer saturation on overflow with the full
token consumed, float tokens through the exact strtod below,
scansets, suppression, width limits on every conversion including
floats, and %n reporting the net consumed count. One honest limit:
a float token is capped at 1200 characters while being collected;
a longer token is still consumed in full from the input, but its
tail is folded into the sticky flag rather than the digit buffer,
which can cost the last ulp on inputs no real parser produces
(thousands of significant digits in one scanf float).

## strtod: exact, with the host's errno rule

strtod / strtof / atof are exactly rounded in both the decimal and
hexadecimal forms, by the same bigint machinery run in reverse
(Eisel-Lemire-style fast paths were deliberately not used: the
exact path is fast enough at engine parse rates and has no
fallback boundary to audit). ERANGE follows the host's rule,
verified by the differential gate: overflow to infinity raises it;
underflow raises it ONLY when the conversion was inexact at the
subnormal grid (an exact subnormal like 0x1p-1074 converts
silently). Exact ties round to even; a sticky tail (digits beyond
the kept prefix, or a nonzero division remainder) breaks a tie
upward. The kept prefix is 800 digits, comfortably past the
767-digit bound past which a double's rounding is already decided,
so the sticky argument is sound: beyond digit 800 nothing can flip
a rounding that 800 digits have not.

## stdlib notes

* strtol family: full base grammar (0x consumed only when a hex
  digit follows), saturation to the limit with ERANGE, endptr
  exact in all cases including the no-digits case.
* qsort is an introsort (median-of-three pivot, three-way
  partition, insertion sort under 16 elements, heapsort fallback
  past the depth cap). Chosen over mergesort for worst-case safety
  on big engine arrays: O(n log n) worst case with NO allocation,
  so sorting cannot fail inside a memory-tight frame. bsearch is a
  plain binary search.
* rand / srand: splitmix64. It is a fine general-purpose generator
  and it must NEVER feed the deterministic simulation: the sim has
  its own seeded machinery, and this one's contract is only
  "statistically decent, reproducible per seed". (One documented
  divergence: srand adds 1 to the seed state, so our sequence does
  not match glibc's srand(1) equivalence class.)
* exit / _Exit / quick_exit / abort: exit() runs
  __cxa_finalize(NULL), which drains the single unified registry
  that atexit() feeds (atexit forwards to __cxa_atexit, the musl
  arrangement, so C and C++ handlers interleave in true LIFO
  order), then flushes stdio, then lithosys_exit. _Exit skips the
  registry and the flush. quick_exit has its own 32-entry registry
  per the standard. abort() makes a best-effort stdio flush, then
  lithosys_abort.
* getenv reads an in-module table that is EMPTY by default, per
  the freeze (the engine's LL_* flags therefore read as unset in
  the wasm build unless the platform injects). An internal setter,
  __litholibc_env_inject, exists for tests and platform injection;
  it is not declared in stdlib.h.
* Multibyte: mblen / mbtowc / wctomb implement the C locale only
  (bytes above 0x7f are EILSEQ), which is all the engine's text
  paths assume today.

## math: what is implemented, and how accurate it is

Portable implementations written for this library (fdlibm-class
algorithms rewritten from their mathematical descriptions, musl
studied as the reference of record). The elementary set is exact
by construction: sqrt by a 106-bit integer square root with the
exact rounding rule (round up iff the remainder exceeds the root);
floor family by mantissa masking; fmod and remainder by exact
bigint division, remainder's ties going to the even quotient.
The rest are series with argument reduction. Measured worst-case
errors against the host libm, from tests/libc_stdio_test.c sweeps
(the gate asserts inside these bounds):

| Function | Worst | Notes |
|---|---|---|
| log, log2, log10 | 1 ulp | atanh series, hi/lo ln2 split |
| exp | 3 ulp | hi/lo reciprocal reduction |
| log near 1 | 3 ulp | the log1p-shaped region of log |
| sin, cos | 1 ulp | Cody-Waite 3-part pi/2, Dekker products; exact-reduction domain \|x\| < 2^31 * (pi/2), NaN with EDOM beyond it (audit fix B-2) |
| tan | 2 ulp | through the sin/cos kernels; same domain |
| asin | 4 ulp | (1-x)(1+x) product form against cancellation |
| acos | 3 ulp | same |
| atan | 3 ulp | adaptive halvings to 0.1, then Taylor |
| atan2 | 1 ulp | |
| hypot | 2 ulp | scaled, exact sqrt |
| pow, integral exponents | 1 ulp | double-double binary powering, |y| <= 4096 |
| pow, general | 13 ulp | double-double exponent (below) |

pow deserves its sentence: a single-double exponent
(exp(y * log x)) measured 123 ulp worst on the gate's sweep,
because log's last bit is amplified by the exponent's magnitude.
The general path therefore forms y * log2(x) in double-double
(split log2 from the log core, Dekker residuals on both products)
and evaluates 2^X with the fractional part carried separately;
that is what brings it to 13. Integral exponents never take that
path. The float variants compute in double and round once.

Classification (isnan, isinf, fpclassify, signbit) lives in
math.h as macros over two small functions, per C99. No long
double variants exist.

## time: UTC by the freeze

time() reads lithosys_clock_now_ns's realtime clock; clock()
reads the monotonic clock in microseconds with CLOCKS_PER_SEC
1,000,000, documented in time.h. gmtime and localtime are the
SAME conversion (UTC), because the freeze fixes civil time to
UTC and leaves the timezone question open: no offset call was
added. Broken-down conversion is Hinnant's era arithmetic,
exact across the full time_t range the tests exercise (year 0
through 9999). mktime normalizes out-of-range fields the
standard way. strftime implements the sane set the shapes above
need (%Y %m %d %H %M %S %a %j %F %T %p %I and friends, %Z "UTC",
%z "+0000"). One known, accepted divergence: for years below
100, %Y zero-pads to four digits here ("0000" for the year 0)
where glibc prints unpadded ("0"); the ISO shape was kept.

## new/delete: LithoAbi's debt, paid

src/libc/new_delete.cc provides the full family over LithoLibC
malloc / aligned_alloc: scalar and array, throwing and nothrow,
placement, sized delete, and the C++17 aligned forms. The two
symbols LithoAbi's own objects referenced but left undefined,
_ZnwmPv (placement new) and _ZdlPvm (sized delete), are defined
here; the joint link is proven by tests/libc_new_delete_test.cpp,
which links the LithoAbi objects and this family together.
Throwing forms run the new-handler loop over
__cxxabiv1::__cxa_new_handler and then throw through LithoAbi's
designated entry, __cxxabiv1::lithoabi_throw_bad_alloc(), so
bad_alloc arrives with LithoAbi's emergency-pool discipline
(slot 7 reserved) intact.

Two integration facts the joint link taught, recorded here
because they are exactly the kind that bite twice:

* std::nothrow is defined by LithoAbi's std_exceptions.cc, NOT
  here. new_delete.cc consumes the extern declaration only; an
  early draft defined it in both places and the joint link
  refused the duplicate.
* One header debt remains, and it is a constraint artifact, not
  an oversight: this piece was allowed to modify exactly one
  pre-existing file (lithosys.h), so <new> still does not
  DECLARE the operator family or std::set_new_handler /
  std::get_new_handler, even though new_delete.cc defines them
  all (the header's own comment anticipated this widening).
  Until a header pass is authorized, consumers see the operators
  through the compiler's built-in knowledge of the language
  forms, and set_new_handler through local declaration, which
  is what the smoke test does. The definitions link and run
  today; only the declarations wait.

## The lkboot volume integration decision (presented, not decided)

The volume call is frozen; how lkboot serves it is an integration
decision for the toolchain phase, and this piece deliberately
does not make it. Both shapes:

* Shape A, host-parsed index: lkboot fetches the .data pack,
  parses its LKP2 directory itself, and keeps a path-to-range
  table on the page side. lithosys_volume_open looks the path up
  in that table and mints an fd over the byte range. Pro: the
  module never learns the pack format; the table is built once
  at fetch time. Con: the LKP2 directory format gains a second
  parser, in JavaScript, that must agree with the engine's.
* Shape B, module-supplied index: the module parses its own
  pack (it already can) and registers the resulting index with
  the host through a channel the toolchain phase defines;
  volume_open then resolves against registered ranges. Pro: one
  parser of record, in the engine. Con: needs a registration
  channel that does not exist in the frozen surface, so it
  arrives with whatever the toolchain phase adds for it.

The stdio layer above the call is identical under both shapes.
The native test double (src/lithosys_volume_native.c) is neither:
it serves a host directory under a root set by
lithosys_volume_native_set_root, rejecting absolute paths and
".." components with EPERM, and exists only so the FILE layer
could be gated on a host.

## Test evidence

tests/libc_stdio_test.c, 25 groups (23 from the piece as landed
plus 2 audit regression groups from the 2026-10-28 fix run), all
differential against the
host libc and libm fetched by dlsym (verdicts print through the
host's vsnprintf, so the formatter under test never grades
itself): printf integers, strings, chars; printf floats including
4000 random bit patterns through %.17g / %g / %.9e; mixed formats,
star arguments, %n, return values; FILE reads through the volume
double (fread chunks, fseek, EOF, fgets, fscanf "%lu %lu"); the
fail-soft table (EROFS on write-to-volume, ENOSYS with no volume,
ENOENT on a miss, remove/rename ENOSYS); stdout/stderr routing
with fd capture and a line-buffering probe; scanf against the
census shapes including scansets and the dangling-exponent case;
the strtol family (34 cases, saturation, ERANGE, endptr); strtod
bit-exact with errno and endptr over fixed cases (including
900-digit sticky inputs) and 3000 random decimal strings; qsort
against the host on sizes 0 to 30000 across five patterns plus
bsearch; stdlib misc (rand determinism, getenv injection, system
ENOSYS, multibyte C locale); the math sweeps tabled above with
the exact families bitwise; time against the host (fields,
strftime shapes, mktime round trips and normalization).

tests/libc_new_delete_test.cpp, 8 groups: scalar/array with
ctor/dtor counts; placement new identity; sized delete; aligned
new/delete; nothrow failure and the handler accessors; and three
forked-child proofs: the new-handler runs on throwing-new
failure (byte plus exit code), an uncaught bad_alloc kills the
child with SIGABRT through LithoAbi's terminate path, and exit()
drains the unified atexit registry in LIFO order and leaves
with the requested code 7.

Both suites pass plain. The C suite also passes under ASan and
UBSan with no report from either sanitizer, in the
symbol-prefixed arrangement Pieces 3 to 5 established: every
public and cross-file name of the library renamed lk_* via -D,
uniformly across library and test so nothing interposes, with
src/libc/malloc.c and src/lithothread_errno.c compiled without
instrumentation per the loader-path recipe. The rename set is
mechanical: the defined globals of the plain objects (nm -g
--defined-only over the piece's and core's objects), 216 names.
Regenerate it with:

```sh
nm -g --defined-only <plain objects> | awk '{print $3}' \
  | sort -u   # then -D<name>=lk_<name> for each
```

(The one symbol excluded is lt__current_rec, LithoThread's own
internal, which is not a libc name.)

## Bugs the gate caught in this piece

Found by the differential runs and fixed during the piece, in
conviction order:

* bigint shift-left dropped the carry word at the top target
  (its guard covered both terms with src < n, so the carry out
  of the old top word never landed). One defect, three symptom
  families: truncated fmod/remainder results at wide exponent
  spreads, dtoa expansions of large values, strtod alignments.
* sqrt's integer loop ran 53 pair-steps over a 128-bit container
  whose radicand is anchored at the bottom (106 significant
  bits): the first 11 steps consumed zero pairs and every root
  came out 2^11 small in exponent and short in mantissa. It also
  explained the apparent asin/acos/atan/hypot failures, which
  were this bug arriving through sqrt. 64 steps now.
* asin/acos formed 1 - x*x and cancelled catastrophically near
  |x| = 1 (301 ulp at 0.99995); the product form (1-x)(1+x)
  measures 4 and 3 ulp.
* atan halved a fixed three times, charging tiny arguments three
  roundings the rescale then amplified (5 ulp); the halvings are
  adaptive now (3 ulp worst).
* pow's first form (see above): 123 ulp general, 18 ulp integral
  by plain binary powering; the double-double exponent and
  double-double powering measure 13 and 1.
* strtod raised ERANGE for every subnormal result; the host
  raises it only for inexact ones. Fixed in both finish paths
  (see the errno rule above).
* std::nothrow defined twice across the piece boundary (above).

## Honest remaining risks

* pow's general path at 13 ulp worst is the loosest number in
  the library. It is documented in math.h and in math.c rather
  than rounded down in the telling. If a future engine system
  needs better, the next step is a double-double exp kernel,
  not more sweep luck.
* scanf's float token cap (1200 chars) is described above; the
  last-ulp exposure needs multi-thousand-digit scanf floats,
  which no engine input produces.
* strtod's sticky argument rests on the 800-digit kept prefix
  versus the 767-digit decision bound. The bound argument is
  standard, and the gate's 900-digit cases pass, but the proof
  is the literature's, not a new one.
* The exit/abort interposition loop the smoke test documents
  (native back-end calling host exit/abort inside a binary that
  interposes them) is a bench artifact of the test doubles, not
  a product path; it is recorded in the smoke's build notes so
  the next joint link does not rediscover it as a segfault.
* Wasm-side verification of this whole piece is still owed, as
  it is for every LithoRoot piece: the host gates prove the
  portable logic, and the throw path's wasm shape (tags,
  landing pads) belongs to the toolchain piece that follows.
* The <new> declaration debt (above) needs one authorized
  header pass; until then the operator declarations travel by
  the compiler's language knowledge and local declarations.

## Gate notes (Elfie, 2026-10-28, independent rebuild)

Rebuilt from source in a fresh directory and re-ran everything.
The plain C suite passes ALL GROUPS (exit 0) with the piece's own
sources compiled fresh under -std=c99 -Wall -Wextra -Werror
-pedantic; the measured ulp figures reproduce exactly as tabled
above (log 1, exp 3, asin 4, pow general 13, pow integral 1,
atan2 1, hypot 2, exact families zero mismatches in 4000). The
new/delete smoke passes 8/8 linked with a fresh new_delete.o
against the LithoAbi objects, death proofs included. The ASan
plus UBSan run of the C suite, rebuilt with the documented
216-name prefix set, passes ALL GROUPS with zero sanitizer
reports. Support objects for the previously gated pieces
(WaferHeap, LibC core, Weave, Thread host side) were reused from
their own gate builds; everything this piece claims as its own
was recompiled from source for this gate.

One build-recipe fact: src/libc/math.c trips GCC's
-Winfinite-recursion on the sqrtf wrapper (sqrtf calls the
library's own double sqrt, a different function; the warning is
the compiler's builtin-name false positive). Under -Werror,
demote that single warning for this file
(-Wno-error=infinite-recursion); the warning itself stays
visible. The test file's snprintf truncation warning is an
intentional test case, the same class as Piece 3's test-file
pedantic note.
