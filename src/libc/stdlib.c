/* stdlib.c ... LithoLibC general utilities (the allocation
 * family lives in malloc.c; the float conversions in strtod.c).
 *
 * Contents and the calls behind them:
 *   strtol family: hand-rolled over the WASI-numbered errno set,
 *     with saturation and ERANGE on overflow, and the standard's
 *     subject-sequence rules (base 0 prefixes, "0x" consumed only
 *     when a hex digit follows).
 *   abs / div families: trivial, but the engine calls them
 *     constantly through std:: wrappers that land here.
 *   qsort: introsort (median-of-three quicksort, introspection
 *     depth cap, heapsort fallback, insertion sort for short
 *     runs). Chosen over mergesort because its worst case is
 *     O(n log n) with NO allocation: a mergesort that cannot
 *     allocate a scratch buffer on a nearly-full WaferHeap would
 *     have to fail or degrade, and qsort has no void return to
 *     hide a failure in anyway. Element moves go through a
 *     fixed stack buffer in chunks.
 *   bsearch: plain binary search.
 *   rand / srand: splitmix64. State is process-global and
 *     mutable, so rand must NEVER feed the deterministic
 *     simulation: the engine's sim runs on its own seeded
 *     streams, and the census agrees (the only rand mentions in
 *     the tree are comments saying not to use it). It exists for
 *     tools and tests that want a cheap shuffle.
 *   exit family: exit() runs the registered handlers through
 *     LithoAbi's table (plain atexit forwards to __cxa_atexit,
 *     exactly as musl's does: the C and C++ registries are one
 *     registry), then flushes stdio, then lithosys_exit.
 *     quick_exit runs only the at_quick_exit registry (its own
 *     small list) and does not flush. _Exit goes straight to
 *     lithosys_exit. abort() flushes best-effort and calls
 *     lithosys_abort.
 *   getenv: the freeze's environment is an empty in-module table;
 *     getenv consults the injected table (empty by default, so
 *     every LL_* feature flag reads as unset on the web host,
 *     matching today's behavior) and otherwise returns NULL.
 *   system: no command processor exists on any frozen host:
 *     returns -1 with ENOSYS, always (fail soft, documented).
 *   mblen / mbtowc / wctomb: the only locale is the C locale, so
 *     these are the byte-wise definitions; a byte above 0x7f in
 *     mbtowc / mblen is EILSEQ, as the C locale requires.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "litholibc_internal.h"
#include "lithosys.h"
#include "lithoweave.h" /* lw_run_tls_destructors, for exit() */

/* LithoAbi entry points used by the exit family. Declared here
 * rather than through a shared header: LithoAbi is a separate
 * library and this is its stable C surface for our purposes. */
int __cxa_atexit(void (*func)(void*), void* arg, void* dso);
void __cxa_finalize(void* dso);

/* ---- strtol family ------------------------------------------------- */

static const char* skip_ws_sign(const char* s, int* neg) {
  while (isspace((unsigned char)*s)) s++;
  *neg = 0;
  if (*s == '+' || *s == '-') {
    *neg = (*s == '-');
    s++;
  }
  return s;
}

static int digit_val(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

/* Shared scan: returns the accumulated magnitude (saturating at
 * `limit`), sets *any, *overflow, and *end. The caller applies
 * sign and the target type's range rules. */
static unsigned long long scan_int(const char* s, char** endptr, int base,
                                   int neg, unsigned long long limit,
                                   int* any_out, int* ovf_out) {
  const char* p = s;
  unsigned long long acc = 0;
  int any = 0, ovf = 0;

  if (base < 0 || base == 1 || base > 36) {
    errno = EINVAL;
    if (endptr) *endptr = (char*)s;
    *any_out = 0;
    *ovf_out = 0;
    return 0;
  }
  if (base == 0) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit_val((unsigned char)p[2]) >= 0 && digit_val((unsigned char)p[2]) < 16) {
      base = 16;
      p += 2;
    } else if (p[0] == '0') {
      base = 8;
    } else {
      base = 10;
    }
  } else if (base == 16) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        digit_val((unsigned char)p[2]) >= 0 && digit_val((unsigned char)p[2]) < 16) {
      p += 2;
    }
  }
  (void)neg;
  {
    unsigned long long cutoff = limit / (unsigned)base;
    unsigned long long cutlim = limit % (unsigned)base;
    const char* q = p;
    while (1) {
      int d = digit_val((unsigned char)*q);
      if (d < 0 || d >= base) break;
      any = 1;
      if (!ovf) {
        if (acc > cutoff || (acc == cutoff && (unsigned long long)d > cutlim)) {
          ovf = 1;
          acc = limit;
        } else {
          acc = acc * (unsigned)base + (unsigned)d;
        }
      }
      q++;
    }
    if (endptr) *endptr = (char*)(any ? q : s);
  }
  *any_out = any;
  *ovf_out = ovf;
  return acc;
}

long strtol(const char* s, char** endptr, int base) {
  int neg, any, ovf;
  const char* p = skip_ws_sign(s, &neg);
  unsigned long long limit =
      neg ? (unsigned long long)LONG_MAX + 1u : (unsigned long long)LONG_MAX;
  unsigned long long acc = scan_int(p, endptr, base, neg, limit, &any, &ovf);
  if (endptr && !any) *endptr = (char*)s;
  if (!any) return 0;
  if (ovf) {
    errno = ERANGE;
    return neg ? LONG_MIN : LONG_MAX;
  }
  if (neg) {
    if (acc == (unsigned long long)LONG_MAX + 1u) return LONG_MIN;
    return -(long)acc;
  }
  return (long)acc;
}

long long strtoll(const char* s, char** endptr, int base) {
  int neg, any, ovf;
  const char* p = skip_ws_sign(s, &neg);
  unsigned long long limit =
      neg ? (unsigned long long)LLONG_MAX + 1u : (unsigned long long)LLONG_MAX;
  unsigned long long acc = scan_int(p, endptr, base, neg, limit, &any, &ovf);
  if (endptr && !any) *endptr = (char*)s;
  if (!any) return 0;
  if (ovf) {
    errno = ERANGE;
    return neg ? LLONG_MIN : LLONG_MAX;
  }
  if (neg) {
    if (acc == (unsigned long long)LLONG_MAX + 1u) return LLONG_MIN;
    return -(long long)acc;
  }
  return (long long)acc;
}

unsigned long strtoul(const char* s, char** endptr, int base) {
  int neg, any, ovf;
  const char* p = skip_ws_sign(s, &neg);
  unsigned long long acc =
      scan_int(p, endptr, base, neg, ULONG_MAX, &any, &ovf);
  if (endptr && !any) *endptr = (char*)s;
  if (!any) return 0;
  if (ovf) {
    errno = ERANGE;
    return ULONG_MAX;
  }
  if (neg) return (unsigned long)(0ul - (unsigned long)acc);
  return (unsigned long)acc;
}

unsigned long long strtoull(const char* s, char** endptr, int base) {
  int neg, any, ovf;
  const char* p = skip_ws_sign(s, &neg);
  unsigned long long acc =
      scan_int(p, endptr, base, neg, ULLONG_MAX, &any, &ovf);
  if (endptr && !any) *endptr = (char*)s;
  if (!any) return 0;
  if (ovf) {
    errno = ERANGE;
    return ULLONG_MAX;
  }
  if (neg) return 0ull - acc;
  return acc;
}

int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }
long long atoll(const char* s) { return strtoll(s, NULL, 10); }

/* ---- abs / div families ---------------------------------------------- */

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

div_t div(int num, int den) {
  div_t r;
  r.quot = num / den;
  r.rem = num - r.quot * den;
  return r;
}
ldiv_t ldiv(long num, long den) {
  ldiv_t r;
  r.quot = num / den;
  r.rem = num - r.quot * den;
  return r;
}
lldiv_t lldiv(long long num, long long den) {
  lldiv_t r;
  r.quot = num / den;
  r.rem = num - r.quot * den;
  return r;
}

/* ---- qsort: introsort -------------------------------------------------- */

static void swap_elems(char* a, char* b, size_t size) {
  char buf[256];
  if (a == b) return;
  while (size > 0) {
    size_t chunk = size < sizeof buf ? size : sizeof buf;
    memcpy(buf, a, chunk);
    memcpy(a, b, chunk);
    memcpy(b, buf, chunk);
    a += chunk;
    b += chunk;
    size -= chunk;
  }
}

static void insertion_sort(char* base, size_t n, size_t size,
                           int (*cmp)(const void*, const void*)) {
  size_t i, j;
  for (i = 1; i < n; i++) {
    /* Move element i left while it is smaller than its neighbor,
     * swapping as we go (no scratch element buffer needed). */
    j = i;
    while (j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0) {
      swap_elems(base + (j - 1) * size, base + j * size, size);
      j--;
    }
  }
}

static void sift_down(char* base, size_t root, size_t n, size_t size,
                      int (*cmp)(const void*, const void*)) {
  while (1) {
    size_t child = 2 * root + 1;
    if (child >= n) return;
    if (child + 1 < n &&
        cmp(base + child * size, base + (child + 1) * size) < 0) {
      child++;
    }
    if (cmp(base + root * size, base + child * size) < 0) {
      swap_elems(base + root * size, base + child * size, size);
      root = child;
    } else {
      return;
    }
  }
}

static void heap_sort(char* base, size_t n, size_t size,
                      int (*cmp)(const void*, const void*)) {
  size_t i;
  if (n < 2) return;
  for (i = n / 2; i-- > 0;) sift_down(base, i, n, size, cmp);
  for (i = n - 1; i > 0; i--) {
    swap_elems(base, base + i * size, size);
    sift_down(base, 0, i, size, cmp);
  }
}

#define QSORT_INSERTION_MAX 16

static void introsort(char* base, size_t n, size_t size,
                      int (*cmp)(const void*, const void*), int depth) {
  while (n > QSORT_INSERTION_MAX) {
    char *mid, *lo, *hi, *piv_store;
    size_t lt, i, gt;
    if (depth <= 0) {
      heap_sort(base, n, size, cmp);
      return;
    }
    depth--;
    /* Median of three into the middle slot, then a three-way
     * (Dutch flag) partition around it: arrays with many equal
     * keys collapse in one pass instead of degrading. */
    mid = base + (n / 2) * size;
    {
      char* first = base;
      char* last = base + (n - 1) * size;
      if (cmp(mid, first) < 0) swap_elems(mid, first, size);
      if (cmp(last, mid) < 0) {
        swap_elems(last, mid, size);
        if (cmp(mid, first) < 0) swap_elems(mid, first, size);
      }
    }
    piv_store = mid;
    lt = 0;
    i = 0;
    gt = n;
    /* Partition [0,lt) < piv, [lt,i) == piv... the pivot element
     * itself rides at piv_store; standard three-way scan with the
     * pivot value compared through its slot. To keep the pivot
     * stable during swaps, first move it to the front. */
    swap_elems(base, piv_store, size);
    piv_store = base;
    lt = 1;
    i = 1;
    gt = n;
    while (i < gt) {
      int c = cmp(base + i * size, piv_store);
      if (c < 0) {
        swap_elems(base + lt * size, base + i * size, size);
        lt++;
        i++;
      } else if (c > 0) {
        gt--;
        swap_elems(base + i * size, base + gt * size, size);
      } else {
        i++;
      }
    }
    /* [0, lt) now holds elements <= piv with the == band at
     * [lt - 1 + ... ]... precisely: elements < piv occupy
     * [1, lt) after the pivot at [0]; the == band is folded into
     * the left segment's tail. Recurse on the strictly-less left
     * part [0, lt) including the pivot (harmless: it is already
     * placed correctly relative to the right part), and iterate
     * on the strictly-greater right part [gt, n). The == band
     * [lt, gt) is finished. */
    introsort(base, lt, size, cmp, depth);
    base += gt * size;
    n -= gt;
    (void)lo;
    (void)hi;
  }
  insertion_sort(base, n, size, cmp);
}

void qsort(void* base_, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*)) {
  int depth = 0;
  size_t m;
  if (nmemb < 2 || size == 0) return;
  for (m = nmemb; m > 1; m >>= 1) depth++;
  introsort((char*)base_, nmemb, size, compar, 2 * depth);
}

void* bsearch(const void* key, const void* base_, size_t nmemb, size_t size,
              int (*compar)(const void*, const void*)) {
  const char* base = (const char*)base_;
  size_t lo = 0, hi = nmemb;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = compar(key, base + mid * size);
    if (c < 0) {
      hi = mid;
    } else if (c > 0) {
      lo = mid + 1;
    } else {
      return (void*)(base + mid * size);
    }
  }
  return NULL;
}

/* ---- rand: splitmix64 -------------------------------------------------- */

static uint64_t g_rand_state = 1;

void srand(unsigned seed) { g_rand_state = (uint64_t)seed + 1u; }

int rand(void) {
  /* splitmix64: a well-distributed 64-bit generator with a tiny
   * state, chosen because it needs no table and no seed ceremony.
   * The +1 in srand and the seeded-with-1 default make rand()
   * after srand(1) differ from the unseeded stream; glibc's
   * srand(1) == unseeded equivalence is a glibc quirk, not a
   * standard requirement, and our tests do not depend on it. */
  uint64_t z = (g_rand_state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  z = z ^ (z >> 31);
  return (int)(z >> 33) & RAND_MAX;
}

/* ---- exit family -------------------------------------------------------- */

int atexit(void (*func)(void)) {
  /* One registry for C and C++ (LithoAbi's), as in musl: the
   * handler adapts to the registry's (void (*)(void*), arg) shape
   * with a NULL argument. */
  return __cxa_atexit((void (*)(void*))func, NULL, NULL);
}

#define QUICK_EXIT_MAX 32
static void (*g_quick_exit[QUICK_EXIT_MAX])(void);
static int g_quick_exit_count = 0;

int at_quick_exit(void (*func)(void)) {
  if (g_quick_exit_count >= QUICK_EXIT_MAX) return -1;
  g_quick_exit[g_quick_exit_count++] = func;
  return 0;
}

void exit(int code) {
  /* The C standard's sequence: handlers (LIFO, inside
   * __cxa_finalize(NULL) covering every registered dso handle,
   * per LithoAbi's implementation), then flush, then leave.
   * (The never-returns attribute rides on the stdlib.h
   * declaration; it is not repeated on the definitions.) */
  __cxa_finalize(NULL);
  /* Audit fix E-1: the calling thread's TLS destructors are not
   * part of the finalize registry. They live on the weave's
   * per-thread list (LithoAbi's __cxa_thread_atexit registers
   * there), and while a spawned thread's exit runs them through
   * the trampoline, nothing ran the MAIN thread's on this path:
   * a main-thread thread_local's destructor silently never ran.
   * Run them here, between finalize and the flush. The runner
   * is a no-op when nothing is registered, and its exiting
   * guard keeps a destructor that itself reaches exit() from
   * re-entering the pass. */
  lw_run_tls_destructors();
  __litholibc_stdio_flush_all();
  lithosys_exit(code);
}

void quick_exit(int code) {
  int i;
  for (i = g_quick_exit_count - 1; i >= 0; i--) g_quick_exit[i]();
  lithosys_exit(code);
}

void _Exit(int code) { lithosys_exit(code); }

void abort(void) {
  /* Flush is best-effort: the process is dying, and the flush
   * path is not allowed to turn an abort into a hang. */
  __litholibc_stdio_flush_all();
  lithosys_abort();
}

/* ---- environment --------------------------------------------------------- */

#define ENV_MAX 64
static const char* g_env[ENV_MAX];
static int g_env_count = 0;

void __litholibc_env_inject(const char* name_eq_value) {
  if (g_env_count < ENV_MAX && name_eq_value != NULL) {
    g_env[g_env_count++] = name_eq_value;
  }
}

char* getenv(const char* name) {
  size_t nlen;
  int i;
  if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) return NULL;
  nlen = strlen(name);
  for (i = 0; i < g_env_count; i++) {
    const char* e = g_env[i];
    if (strncmp(e, name, nlen) == 0 && e[nlen] == '=') {
      return (char*)(e + nlen + 1);
    }
  }
  return NULL;
}

/* ---- system ---------------------------------------------------------------- */

int system(const char* cmd) {
  (void)cmd;
  errno = ENOSYS;
  return -1;
}

/* ---- minimal C-locale multibyte -------------------------------------------- */

int mblen(const char* s, size_t n) {
  if (s == NULL) return 0; /* stateless encoding */
  if (n == 0) return -1;
  if (*s == '\0') return 0;
  if ((unsigned char)*s > 0x7f) {
    errno = EILSEQ;
    return -1;
  }
  return 1;
}

int mbtowc(wchar_t* pwc, const char* s, size_t n) {
  if (s == NULL) return 0;
  if (n == 0) return -1;
  if (*s == '\0') {
    if (pwc) *pwc = 0;
    return 0;
  }
  if ((unsigned char)*s > 0x7f) {
    errno = EILSEQ;
    return -1;
  }
  if (pwc) *pwc = (wchar_t)(unsigned char)*s;
  return 1;
}

int wctomb(char* s, wchar_t wc) {
  if (s == NULL) return 0;
  if (wc < 0 || wc > 0x7f) {
    errno = EILSEQ;
    return -1;
  }
  *s = (char)wc;
  return 1;
}
