/* inttypes.h ... LithoLibC (S2 L2, 2026-11-16). The PRI and SCN macros
 * for wasm32 (int32 = int, int64 = long long, intptr = long) plus
 * imaxabs/imaxdiv/strtoimax/strtoumax over the long long forms.
 * Clang's own inttypes.h does #include_next, so this one replaces it. */
#ifndef LITHOLIBC_INTTYPES_H
#define LITHOLIBC_INTTYPES_H
#if !defined(__wasm__)
#error "LithoLibC inttypes.h is wasm32-only"
#endif
#include <stdint.h>
#include <stdlib.h>
#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIx8 "x"
#define PRIX8 "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRIo32 "o"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIo64 "llo"
#define PRIdMAX "lld"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"
#define SCNd32 "d"
#define SCNu32 "u"
#define SCNx32 "x"
#define SCNd64 "lld"
#define SCNu64 "llu"
#define SCNx64 "llx"
typedef struct { intmax_t quot, rem; } imaxdiv_t;
static inline intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }
static inline imaxdiv_t imaxdiv(intmax_t n, intmax_t d) { imaxdiv_t r = {n / d, n % d}; return r; }
static inline intmax_t strtoimax(const char* s, char** e, int b) { return strtoll(s, e, b); }
static inline uintmax_t strtoumax(const char* s, char** e, int b) { return strtoull(s, e, b); }
#endif
