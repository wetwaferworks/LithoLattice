/* stdlib.h ... the LithoLibC general utilities header.
 *
 * The allocation family (implemented in src/libc/malloc.c as thin
 * wrappers over WaferHeap's default heap) plus what the
 * stdio / stdlib / math piece adds in src/libc/stdlib.c and
 * strtod.c: string conversions, sorting and searching, the
 * process exit family wired to the LithoAbi / LithoWeave atexit
 * registries and to lithosys_exit, the environment over the
 * freeze's empty in-module table, and minimal C-locale multibyte
 * length functions.
 *
 * Failure contract for the allocation family: a NULL return from
 * malloc, calloc, realloc or aligned_alloc sets errno to ENOMEM
 * (invalid arguments to aligned_alloc set EINVAL instead).
 * Successful calls never write errno. posix_memalign follows its
 * own standard contract: it returns the error number directly and
 * does not touch errno.
 */
#ifndef LITHOLIBC_STDLIB_H
#define LITHOLIBC_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Portability shim for the declarations that never return (the
 * same trick lithosys.h uses). */
#if defined(_MSC_VER)
#define LITHOLIBC_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define LITHOLIBC_NORETURN __attribute__((__noreturn__))
#elif defined(__cplusplus)
#define LITHOLIBC_NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define LITHOLIBC_NORETURN _Noreturn
#else
#define LITHOLIBC_NORETURN
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
/* The only locale is the C locale: one byte per character. */
#define MB_CUR_MAX 1

typedef struct {
  int quot;
  int rem;
} div_t;
typedef struct {
  long quot;
  long rem;
} ldiv_t;
typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

/* ---- allocation family (malloc.c) -------------------------------- */
void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t count, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);
size_t malloc_usable_size(const void* ptr);

/* ---- string conversions (stdlib.c, strtod.c) ---------------------- */
int atoi(const char* s);
long atol(const char* s);
long long atoll(const char* s);
double atof(const char* s);
long strtol(const char* s, char** endptr, int base);
long long strtoll(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
unsigned long long strtoull(const char* s, char** endptr, int base);
double strtod(const char* s, char** endptr);
float strtof(const char* s, char** endptr);

/* ---- arithmetic ----------------------------------------------------- */
int abs(int v);
long labs(long v);
long long llabs(long long v);
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

/* ---- sorting and searching (stdlib.c) ------------------------------- */
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*));

/* ---- pseudo-random (stdlib.c) -----------------------------------------
 * A splitmix64 generator. It must NEVER feed the deterministic
 * simulation: it carries process-global mutable state and its
 * sequence is a convenience, not a contract. The engine's sim has
 * its own seeded streams; see stdlib.c. */
int rand(void);
void srand(unsigned seed);

/* ---- process control (stdlib.c) --------------------------------------- */
int atexit(void (*func)(void));
int at_quick_exit(void (*func)(void));
LITHOLIBC_NORETURN void exit(int code);
LITHOLIBC_NORETURN void quick_exit(int code);
LITHOLIBC_NORETURN void _Exit(int code);
LITHOLIBC_NORETURN void abort(void);

/* ---- environment (stdlib.c) -------------------------------------------
 * Over the freeze's empty in-module table: getenv returns NULL for
 * everything until a native back-end or a test injects entries
 * through the internal hook (litholibc_internal.h). */
char* getenv(const char* name);
/* setenv / unsetenv: POSIX, over a lock-free append-only overlay (at most
 * 64 calls per module, then -1 with ENOMEM); EINVAL for a bad name. */
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);

/* system: no command processor exists on any frozen host. Always
 * fails soft: returns -1 and sets errno to ENOSYS. */
int system(const char* cmd);

/* ---- minimal C-locale multibyte (stdlib.c) ---------------------------- */
int mblen(const char* s, size_t n);
int mbtowc(wchar_t* pwc, const char* s, size_t n);
int wctomb(char* s, wchar_t wc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef LITHOLIBC_NORETURN

#endif /* LITHOLIBC_STDLIB_H */
