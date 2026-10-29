# LithoLibC core design note

Status: PIECE 3 of the LithoRoot program, landed for Elfie's
per-piece gate. Files: public headers `include/string.h`,
`include/strings.h`, `include/ctype.h`, `include/errno.h`,
`include/stdlib.h` (seeded, allocation family only) and
`include/litholibc/errno_values.h`; implementations in
`src/libc/` (memory.c, string.c, strings.c, ctype.c, malloc.c,
errno.c, strerror.c); host test `tests/libc_core_test.c`. Nothing
outside `lithoroot/` was touched, and no Piece 1 or 2 file was
modified. Pieces 1 and 2 are the only dependencies: the malloc
family wraps WaferHeap's `wh_libc_*` default-heap layer, and the
errno numbering follows the LithoSys freeze (see below).

## What is in

- Memory: memcpy, memmove, memset, memcmp, memchr. The copies run
  word at a time once both sides are word aligned. Words move
  through a load/store helper, never through a cast byte pointer:
  on GNU-family compilers the helper is the fixed-size copy
  builtin (lowers to a load or store, can never become a call back
  into memcpy); elsewhere the word is assembled byte by byte.
  memmove picks its direction from the pointer order, and when the
  ranges sit closer than one word it uses a plain byte loop,
  because word steps would overlap each other there.
- String: strlen, strnlen, strcpy, strncpy (with the standard's
  zero padding of the remainder), strcat, strncat, strcmp,
  strncmp, strchr, strrchr, strstr (naive search; needle lengths
  here are short and the naive loop needs no correctness
  argument), strspn, strcspn, strpbrk (over a per-call stack
  bitmap), strtok_r, and strtok over it with the classic static
  save pointer. strdup and strndup live in malloc.c because they
  are allocation functions that happen to copy strings.
- ctype: the full is* set plus tolower and toupper, C locale only,
  from one flag table over the byte range. The table was derived
  mechanically from the class definitions, which are repeated in
  the source above it so the table can be re-derived instead of
  trusted; the host test then verifies every entry against the
  host library anyway. Bytes at or above 0x80 carry no flags in
  the C locale.
- Case-insensitive comparison: strcasecmp, strncasecmp in
  strings.h, folding through our own tolower.
- The malloc family: malloc, free, realloc, calloc,
  aligned_alloc, posix_memalign, malloc_usable_size, as thin
  wrappers over `wh_libc_*`. The wrappers do the one job WaferHeap
  leaves to the libc boundary: a NULL failure becomes ENOMEM in
  errno (invalid alignment becomes EINVAL), successful calls
  never write errno, and posix_memalign follows its own contract
  and returns the error number without touching errno. Two
  inherited behaviors worth knowing: realloc with size 0 frees
  and returns NULL (a success, so no errno), and realloc of an
  over-aligned pointer preserves contents but returns a
  base-aligned pointer, the caveat already documented in
  waferheap.h.
- strerror over our own message table, indexed directly by the
  error number. Unknown codes format into one shared static
  buffer as "Unknown error N"; known codes return literals.
  strerror never writes errno.

## The errno design

errno is a macro over an accessor, `int*
__litholibc_errno_location(void)`, whose current body returns the
address of one static slot. The indirection is the design: when
LithoThread lands, the accessor's body becomes a true TLS lookup
and nothing else moves... no caller ever names a global, so no
caller recompiles and there is no ABI break. The cost is one call
per errno access, paid only by code that touches errno.

The errno VALUES are LithoRoot's own C space, and one decision is
flagged for the gate: the numbering follows the WASI assignments,
not the Linux ones. Piece 1 froze the LithoSys codes in WASI
numbering precisely so translation into the C errno space is a
mechanical pass-through; adopting the same numbers here keeps
that promise with no translation table anywhere. Conforming code
compares errno against the symbolic names and sees no difference.

## Reference-reading discipline

Per the program rule, musl's per-file MIT sources are the
sanctioned reference for this piece: they were read for semantics
and edge cases (the strncpy padding rule, strxfrm's contract,
strtok_r's save-pointer behavior), never copied. Every
implementation here is written fresh in LithoRoot's structure and
comment voice, and the ctype table is derived from the class
definitions rather than lifted. Behavior differences from the
reference that are deliberate are listed in the next section; the
differential host test is the arbiter everywhere else.

## Deliberately out, or deliberately different

- No locale beyond C. strcoll and strxfrm are NOT failure stubs:
  they implement the true C-locale semantics (strcoll compares as
  strcmp; strxfrm copies and returns the full length) and, unlike
  the host's, strxfrm always NUL-terminates when given room. They
  are the seam where a real locale would plug in.
- No wide characters, no multibyte: the wchar surface belongs to
  a later piece if anything ever calls it.
- ctype functions are plain functions, no macro forms, and their
  domain promise is wider than the standard's minimum: defined
  for every int input (EOF classifies as nothing and converts to
  itself; other negatives classify as the byte with the same low
  eight bits; above 255 classifies as nothing). strtok remains
  thread-unsafe by interface, as everywhere; strtok_r is the
  reentrant form.
- Header prototypes carry no `restrict` qualifiers. The C++
  compilers in our matrix reject plain `restrict` in headers, the
  existing LithoRoot headers use none, and the implementations do
  not rely on it. The aliasing promises are the caller's, as in
  the standard text, just not spelled in the declarations.
- aligned_alloc does not enforce the C11 rule that size be a
  multiple of alignment; no caller is served by failing such a
  request, and the wider libc world does not enforce it either.
- Not provided: strerror_r, memccpy, the ffs family, strsignal,
  and everything from stdio, stdlib beyond allocation, and math.

## What the later stdio / stdlib / math piece still owes

- stdlib proper: exit, atexit and the quick-exit family wired to
  lithosys_exit; the string conversions (strtol and family, atoi,
  strtod); qsort and bsearch; the environment (getenv over
  whatever the host story becomes); rand. The seeded stdlib.h
  grows to hold them.
- stdio over the frozen LithoSys fd surface, with the fail-soft
  contract honored (fd_seek says ESPIPE on the web host, stdin
  reads as permanent EOF).
- Math: the engine's used surface only, long double kept out as
  the dossier directs.
- The WASI-numbered errno space meets its one sharp edge there:
  LithoSys codes pass through identically, as designed.

## Verification (host, this VM)

Commands run from `~/workspace/lk/lithoroot`:

    gcc -std=c99 -pedantic -Wall -Wextra -Werror -I include -c src/libc/*.c
    g++ -std=c++11 -Wall -Wextra -Werror -I include header check of all new headers
    gcc -std=c99 -Wall -Wextra -Werror -I include tests/libc_core_test.c \
        src/libc/*.c src/waferheap.c -o /tmp/libc_core_test -ldl && /tmp/libc_core_test

Results: strict C99 compiles clean, no warnings; headers compile
clean from C++11; the test passes all 8 groups (malloc family over
a grow-refusing fixed arena including exhaustion to ENOMEM and
errno preservation on success; strdup and strndup; memory
differential over randomized sizes 0..600 and alignments 0..7;
memmove over 600 randomized overlapping ranges; string
differential over randomized strings; strtok_r sequences;
ctype over the full -1..255 input range against the host C
locale; strerror sanity with distinct messages for all 71 defined
codes plus 0).

Sanitizer runs need a paragraph of their own. The naive ASan
build (library linked into the test executable under its real
names) segfaults before main: the backtrace shows the dynamic
loader calling our malloc during AddressSanitizer's own
interceptor setup (a dlsym error path inside ld.so), and the
instrumented errno store reading shadow memory for the
executable's globals before that shadow is backed. That is a
property of interposing a whole libc surface into an
ASan-instrumented executable on this host, not of the code: the
same binary minus the allocation file runs, and a minimal
interposed-malloc control survives. The sanitizer run therefore
uses a symbol-prefixed build (every public name renamed lk_* via
-D, applied uniformly to the test and the library, so nothing is
interposed and the library itself is fully instrumented):

    gcc -std=c99 -Wall -Wextra -g -fsanitize=address,undefined \
        -Dmemcpy=lk_memcpy ... (50 renames) -I include \
        tests/libc_core_test.c src/libc/*.c src/waferheap.c \
        -o /tmp/libc_asan_prefixed -ldl && /tmp/libc_asan_prefixed

That run passes all 8 groups with no report from either
sanitizer. The exercise caught two real bugs, both in the test,
both fixed: a substring needle that could overflow its 64-byte
buffer for long haystacks, and an strxfrm comparison that read a
truncated result the standard leaves unterminated.

One observation handed up for Piece 2's owner, not fixed here
(Piece 2 files are frozen to this piece): freeing a foreign
pointer whose preceding word falls in an ASan redzone makes
wh_free's decode step read that word before the region check
rejects the pointer (wh_decode, waferheap.c:364, reached from
wh_free). Benign for ordinary memory, and the documented contract
only promises no corruption, but a free at the very start of a
mapped region has the same shape. The core test now exercises the
foreign-free path with a pointer into the middle of a plain
buffer, which is the shape the contract speaks to.
