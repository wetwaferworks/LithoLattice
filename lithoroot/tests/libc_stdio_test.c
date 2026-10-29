/* libc_stdio_test.c ... host functional test for the LithoLibC
 * stdio / stdlib / math / time piece (printf, the FILE layer over
 * the volume, scanf, conversions, qsort, libm, time).
 *
 * Method, following the house pattern of the earlier pieces: the
 * code under test is linked under its real names and checked
 * DIFFERENTIALLY against the host C library and libm, whose entry
 * points are fetched with dlsym on explicit handles (direct calls
 * from this file bind to LithoLibC, which is what is being
 * tested). Verdicts are printed through a dlsym'd host vsnprintf
 * plus write(2), never through the printf under test, so a broken
 * formatter cannot grade itself. The volume is served by the
 * native double (src/lithosys_volume_native.c) over a temp
 * directory fixture; the frozen fd calls are served by
 * tests/lithosys_host_double.c; the stdio mutex binds to
 * LithoThread under the lt_ prefix, exactly as in Piece 5's link.
 *
 * Build (from ~/workspace/lk/lithoroot), plain:
 *
 *   NAMES="(the Piece 5 pthread list; see tests/lithothread_test.c)"
 *   REN=""; for n in $NAMES; do REN="$REN -D$n=lt_$n"; done
 *   for f in lithothread lithothread_errno lithothread_heap; do
 *     gcc -std=c99 -pedantic -Wall -Wextra -I include $REN \
 *         -c src/$f.c -o /tmp/lr/$f.o; done
 *   gcc -std=c99 -pedantic -Wall -Wextra -I include $REN \
 *       -c src/libc/stdio.c -o /tmp/lr/stdio.o
 *   for f in src/lithoweave.c src/waferheap.c \
 *            src/libc/memory.c src/libc/string.c src/libc/strings.c \
 *            src/libc/ctype.c src/libc/malloc.c src/libc/strerror.c \
 *            src/libc/bigint.c src/libc/dtoa.c src/libc/strtod.c \
 *            src/libc/stdlib.c src/libc/math.c src/libc/time.c \
 *            src/libc/printf.c src/libc/scanf.c \
 *            src/lithosys_volume_native.c \
 *            tests/lithosys_host_double.c \
 *            tests/libc_stdio_test.c; do
 *     gcc -std=c99 -pedantic -Wall -Wextra -I include \
 *         -c $f -o /tmp/lr/$(basename $f .c).o; done
 *   gcc -std=c99 -pedantic -Wall -Wextra -c src/lithoweave_native.c \
 *       -o /tmp/lr/lithoweave_native.o
 *   # One bench adjustment, the same one
 *   # tests/libc_new_delete_test.cpp documents: the back-end's
 *   # lithosys_exit / lithosys_abort call the host exit / abort,
 *   # which in this binary are LithoLibC's own (they call
 *   # lithosys_exit / lithosys_abort back: a loop). Rename the
 *   # back-end object's two references; this file defines the
 *   # lk_host_* forwards.
 *   objcopy --redefine-sym exit=lk_host_exit \
 *           --redefine-sym abort=lk_host_abort \
 *           /tmp/lr/lithoweave_native.o
 *   gcc -std=c99 -Wall -Wextra -c src/lithothread_native.c \
 *       -o /tmp/lr/lithothread_native.o
 *   gcc (all objects) -o /tmp/libc_stdio_test -ldl -pthread
 *   /tmp/libc_stdio_test
 *
 * (src/libc/errno.c is deliberately NOT linked: the LithoThread
 * link rule puts lithothread_errno.c in its place.)
 *
 * ASan + UBSan run: the same object plan, with the Piece 3
 * symbol-prefix treatment on top: every public LithoLibC name of
 * this piece and the core (plus the cross-file internals) is
 * renamed lk_* via -D, uniformly across the library and this
 * test, and -g -fsanitize=address,undefined is added to every
 * compile and the link EXCEPT src/libc/malloc.c and
 * src/lithothread_errno.c, which are compiled plain per the
 * loader-path recipe Pieces 3 to 5 documented. The exact rename
 * list is mechanical (the declarations in include/stdio.h,
 * stdlib.h, math.h, time.h, string.h, ctype.h, errno.h plus
 * __litholibc_* and litho_big_* internals) and is spelled out in
 * docs/LITHOLIBC-STDIO.md. The objcopy adjustment above applies
 * there too, against the renamed references:
 * --redefine-sym lk_exit=lk_host_exit and
 * --redefine-sym lk_abort=lk_host_abort.
 */

#include <stdio.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lithosys.h"
#include "waferheap.h"

/* Host services the LithoLibC headers do not declare (the test
 * is compiled against them): declared by hand. */
char* mkdtemp(char* tmpl);
int mkdir(const char* path, unsigned int mode);

/* The volume double's setup hook (src/lithosys_volume_native.c). */
void lithosys_volume_native_set_root(const char* root);
/* The host double's audit hooks (tests/lithosys_host_double.c):
 * arm a one-shot EINTR, or script the underlying write() calls. */
void lithosys_host_double_arm_eintr_write(void);
void lithosys_host_double_arm_eintr_read(void);
void lithosys_host_double_write_script(int cap_call, uint32_t cap,
                                       int fail_call, int zero_call);
int lithosys_host_double_write_calls(void);
/* The weave's TLS destructor registration, for the exit case. */
lithosys_err_t lw_thread_atexit(void (*fn)(void*), void* arg);
/* The internal env injection hook (src/libc/stdlib.c). */
void __litholibc_env_inject(const char* name_eq_value);

/* Aliases for the native back-end's renamed host references
 * (the objcopy step in the build recipe above, the same bench
 * adjustment tests/libc_new_delete_test.cpp documents): the
 * back-end implements lithosys_exit / lithosys_abort BY CALLING
 * the host exit / abort, and in this binary those names are
 * LithoLibC's own, which call lithosys_exit / lithosys_abort
 * back... a loop that ends in stack exhaustion. The audit E-1
 * case is this suite's first exit() caller, which is why the
 * adjustment arrives with it. These definitions forward the
 * renamed references to the host's real functions. */
void lk_host_exit(int code) {
  typedef void (*exit_t)(int);
  static exit_t real_exit = NULL;
  if (real_exit == NULL) {
    void* libc = dlopen("libc.so.6", RTLD_NOW);
    real_exit = (exit_t)(void*)dlsym(libc, "exit");
  }
  real_exit(code);
  _exit(127);
}
void lk_host_abort(void) {
  raise(SIGABRT);
  _exit(134);
}

/* ---- verdict channel: host vsnprintf + write, nothing of ours ---- */
static int (*h_vsnprintf)(char*, size_t, const char*, va_list);
static int (*h_snprintf)(char*, size_t, const char*, ...);
static void report(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = h_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    ssize_t w = write(1, buf, (size_t)n);
    (void)w;
  }
}

static int g_failures = 0;

#define CASE_BEGIN(name) \
  const char* case_name = (name); \
  int case_ok = 1

#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      case_ok = 0;                                        \
      report("  check failed: %s (line %d)\n", #cond,     \
             __LINE__);                                   \
    }                                                     \
  } while (0)

#define CASE_END()                                      \
  do {                                                  \
    report("%s: %s\n", case_ok ? "PASS" : "FAIL",       \
           case_name);                                  \
    if (!case_ok) ++g_failures;                         \
  } while (0)

/* ---- deterministic PRNG (xorshift64) ---- */
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
  uint64_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_rng = x;
  return x;
}
static double rnd_unit(void) {
  return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0);
}

/* ---- host references ---- */
static void* g_libc;
static void* g_libm;
#define HOST_FN(name) \
  (void*)dlsym(g_libc, #name)
#define HOSTM_FN(name) \
  (void*)dlsym(g_libm, #name)

typedef int (*snprintf_t)(char*, size_t, const char*, ...);
typedef long (*strtol_t)(const char*, char**, int);
typedef long long (*strtoll_t)(const char*, char**, int);
typedef unsigned long (*strtoul_t)(const char*, char**, int);
typedef unsigned long long (*strtoull_t)(const char*, char**, int);
typedef double (*strtod_t)(const char*, char**);
typedef int* (*errno_loc_t)(void);
typedef void (*qsort_t)(void*, size_t, size_t,
                        int (*)(const void*, const void*));
typedef double (*math1_t)(double);
typedef double (*math2_t)(double, double);
typedef struct tm* (*gmtime_r_t)(const time_t*, struct tm*);
typedef size_t (*strftime_t)(char*, size_t, const char*, const struct tm*);
typedef FILE* (*hfopen_t)(const char*, const char*);
typedef size_t (*hfwrite_t)(const void*, size_t, size_t, FILE*);
typedef int (*hfclose_t)(FILE*);

static snprintf_t h_snprintf_p;
static strtol_t h_strtol;
static strtoll_t h_strtoll;
static strtoul_t h_strtoul;
static strtoull_t h_strtoull;
static strtod_t h_strtod;
static errno_loc_t h_errno_loc; /* glibc's own errno slot */

/* errno VALUES differ by design: ours are the frozen WASI
 * numbers (Piece 3), glibc's are Linux numbers (ERANGE 34 vs
 * our 68). The contract under test is WHICH condition raises,
 * so the host's ERANGE folds onto ours before comparing. */
static int host_errno_fold(int e) { return e == 34 ? ERANGE : e; }
static qsort_t h_qsort;
static gmtime_r_t h_gmtime_r;
static strftime_t h_strftime;
static hfopen_t h_fopen;
static hfwrite_t h_fwrite;
static hfclose_t h_fclose;

/* ---- ulp machinery ---- */
static int64_t ordered_bits(double d) {
  uint64_t u;
  int64_t i;
  memcpy(&u, &d, sizeof u);
  i = (int64_t)u;
  return i < 0 ? INT64_MIN - i : i;
}
static long long ulp_diff(double a, double b) {
  int64_t x = ordered_bits(a), y = ordered_bits(b);
  long long d = (long long)(x >= y ? x - y : y - x);
  return d;
}
static int dbl_bit_eq(double a, double b) {
  uint64_t x, y;
  if (a != a && b != b) return 1;
  memcpy(&x, &a, sizeof x);
  memcpy(&y, &b, sizeof y);
  return x == y;
}

/* ---- WaferHeap default heap over a static arena ---- */
static unsigned char g_arena[32 * 1024 * 1024];
static void* grow_refused(wh_heap_t* heap, size_t min_extra, size_t* out,
                          void* user) {
  (void)heap;
  (void)min_extra;
  (void)out;
  (void)user;
  return NULL;
}
static void setup_heap(void) {
  wh_options_t opts;
  wh_heap_t* heap;
  memset(&opts, 0, sizeof opts);
  opts.grow = grow_refused;
  heap = wh_heap_create(g_arena, sizeof g_arena, &opts);
  if (heap != NULL) wh_set_default_heap(heap);
}

/* ---- volume fixture ---- */
static char g_root[256];
static unsigned char g_pattern[10000];

static void make_fixtures(void) {
  char tmpl[] = "/tmp/litho_vol_XXXXXX";
  char path[512];
  FILE* f;
  size_t i;
  char* dir = mkdtemp(tmpl);
  if (dir == NULL) {
    report("fixture setup failed\n");
    _exit(2);
  }
  strcpy(g_root, dir);
  for (i = 0; i < sizeof g_pattern; i++) {
    g_pattern[i] = (unsigned char)((i * 131u + 7u) & 0xffu);
  }
  /* hello.txt */
  h_snprintf(path, sizeof path, "%s/hello.txt", g_root);
  f = h_fopen(path, "wb");
  h_fwrite("Hello, volume!\nSecond line.\n", 1, 28, f);
  h_fclose(f);
  /* counts.txt for fscanf */
  h_snprintf(path, sizeof path, "%s/counts.txt", g_root);
  f = h_fopen(path, "wb");
  h_fwrite("1266 1279", 1, 9, f);
  h_fclose(f);
  /* data.bin via a subdirectory */
  {
    char sub[512];
    h_snprintf(sub, sizeof sub, "%s/sub", g_root);
    if (mkdir(sub, 0777) != 0) {
      /* may already exist in a reused dir; mkdtemp says no */
    }
  }
  h_snprintf(path, sizeof path, "%s/sub/data.bin", g_root);
  f = h_fopen(path, "wb");
  h_fwrite(g_pattern, 1, sizeof g_pattern, f);
  h_fclose(f);
  lithosys_volume_native_set_root(g_root);
}

/* ================= printf differentials =================
 * Every case formats the same arguments through our snprintf and
 * the host's and requires byte equality and equal returns. */

static char g_mine[8192], g_host[8192];

#define PFMT1(fmt, a1)                                                \
  do {                                                                \
    int rm = snprintf(g_mine, sizeof g_mine, fmt, a1);                \
    int rh = h_snprintf_p(g_host, sizeof g_host, fmt, a1);            \
    if (rm != rh || strcmp(g_mine, g_host) != 0) {                    \
      case_ok = 0;                                                    \
      report("  fmt %s: mine(%d) [%s] host(%d) [%s]\n", fmt, rm,       \
             g_mine, rh, g_host);                                     \
    }                                                                 \
  } while (0)

#define PFMT0(fmt)                                                    \
  do {                                                                \
    int rm = snprintf(g_mine, sizeof g_mine, fmt);                    \
    int rh = h_snprintf_p(g_host, sizeof g_host, fmt);                \
    if (rm != rh || strcmp(g_mine, g_host) != 0) {                    \
      case_ok = 0;                                                    \
      report("  fmt %s: mine(%d) [%s] host(%d) [%s]\n", fmt, rm,       \
             g_mine, rh, g_host);                                     \
    }                                                                 \
  } while (0)

static void case_printf_integers(void) {
  CASE_BEGIN("printf integers, strings, chars vs host");
  {
    static const int vals[] = {0, 1, -1, 42, -99999, 123456789,
                               2147483647, -2147483647 - 1};
    static const char* const fmts[] = {
        "%d",   "%5d",  "%-5d", "%05d", "%+d",  "% d",  "%i",
        "%5.3d", "%.0d", "%x",  "%#x",  "%#X",  "%o",   "%#o",
        "%u",   "%5u",  "%-8x", "%#08x", "%hd", "%hhd", "%ld"};
    size_t i, j;
    for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
      for (j = 0; j < sizeof vals / sizeof vals[0]; j++) {
        const char* fmt = fmts[i];
        if (strstr(fmt, "%l") == fmt || strstr(fmt, "%l") != NULL) {
          PFMT1(fmt, (long)vals[j]);
        } else if (strstr(fmt, "hh") != NULL) {
          PFMT1(fmt, vals[j]);
        } else if (strstr(fmt, "h") != NULL) {
          PFMT1(fmt, vals[j]);
        } else {
          PFMT1(fmt, vals[j]);
        }
      }
    }
  }
  {
    static const long long llvals[] = {0, -1, 9223372036854775807LL,
                                       -9223372036854775807LL - 1,
                                       123456789012345LL};
    size_t j;
    for (j = 0; j < sizeof llvals / sizeof llvals[0]; j++) {
      PFMT1("%lld", llvals[j]);
      PFMT1("%20lld", llvals[j]);
      PFMT1("%-20llu", (unsigned long long)llvals[j]);
      PFMT1("%llx", (unsigned long long)llvals[j]);
      PFMT1("%zu", (size_t)(llvals[j] < 0
                                ? 0ull - (unsigned long long)llvals[j]
                                : (unsigned long long)llvals[j]));
    }
  }
  PFMT1("%c", 'A');
  PFMT1("%5c", 'z');
  PFMT1("%-5c", '!');
  PFMT1("%s", "hello");
  PFMT1("%10s", "hello");
  PFMT1("%-10s", "hello");
  PFMT1("%.2s", "hello");
  PFMT1("%10.3s", "hello");
  PFMT1("%s", "");
  PFMT1("%p", (void*)NULL);
  PFMT1("%p", (void*)g_mine);
  PFMT0("%%");
  PFMT0("100%% done");
  CASE_END();
}

static void case_printf_floats(void) {
  CASE_BEGIN("printf floats vs host (exact dtoa)");
  {
    static const double vals[] = {
        0.0, -0.0, 1.0, -1.5, 3.141592653589793, 0.1, 2.5, 3.5,
        0.125, 0.375, 0.005, 0.00049999999999999994, 123456.789,
        1e20, 1e21, 1e300, 1e-300, 5e-324, 7.410984687618698e-324,
        0.0001, 0.00001, 100.0, 999999.0, 9999999.0, 1234567.0,
        1.7976931348623157e308, 2.2250738585072014e-308, 0.5, 0.25,
        6.02214076e23, 1.0 / 3.0, 2.0 / 3.0, -273.15, 1e15 + 0.5};
    static const char* const fmts[] = {
        "%f", "%.2f", "%.0f", "%.1f", "%.6f", "%e", "%.3e", "%E",
        "%g", "%.17g", "%.9g", "%G", "%.4g", "%12.4f", "%-12.4f",
        "%012.4f", "%+f", "% f", "%#g", "%5.1e", "%.20f", "%.15e"};
    size_t i, j;
    for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
      for (j = 0; j < sizeof vals / sizeof vals[0]; j++) {
        PFMT1(fmts[i], vals[j]);
      }
    }
  }
  {
    /* Randomized bit patterns through %.17g and %g: the round
     * trip formats. inf and nan are covered by equality of the
     * printed word. */
    int k;
    for (k = 0; k < 4000; k++) {
      uint64_t u = rnd();
      double d;
      memcpy(&d, &u, sizeof d);
      PFMT1("%.17g", d);
      PFMT1("%g", d);
      PFMT1("%.9e", d);
    }
  }
  {
    /* Subnormal-adjacent decimals through %.17g and %f precisions. */
    int k;
    for (k = 0; k < 200; k++) {
      double d = (double)(k + 1) * 5e-324;
      PFMT1("%.17g", d);
      double e = (double)(k + 1) * 1e-310;
      PFMT1("%e", e);
      PFMT1("%f", (double)k * 0.001);
      PFMT1("%.3f", (double)k * 0.0005);
    }
  }
  CASE_END();
}

static void case_printf_mixed(void) {
  CASE_BEGIN("printf mixed formats, star args, %n, returns");
  {
    char a[256], b[256];
    int rm, rh, n1 = -1, n2 = -1;
    rm = snprintf(a, sizeof a, "chunk %d,%d verts=%zu tps=%.2f [%s]",
                  -12, 40, (size_t)4194304, 255.5, "ok");
    rh = h_snprintf_p(b, sizeof b, "chunk %d,%d verts=%zu tps=%.2f [%s]",
                      -12, 40, (size_t)4194304, 255.5, "ok");
    CHECK(rm == rh);
    CHECK(strcmp(a, b) == 0);
    rm = snprintf(a, sizeof a, "%02d:%02d:%02d", 3, 7, 59);
    rh = h_snprintf_p(b, sizeof b, "%02d:%02d:%02d", 3, 7, 59);
    CHECK(rm == rh && strcmp(a, b) == 0);
    rm = snprintf(a, sizeof a, "%*d|%-*s|%.*f", 6, 42, 8, "hi", 3, 1.25);
    rh = h_snprintf_p(b, sizeof b, "%*d|%-*s|%.*f", 6, 42, 8, "hi", 3,
                      1.25);
    CHECK(rm == rh && strcmp(a, b) == 0);
    rm = snprintf(a, sizeof a, "abc%n", &n1);
    rh = h_snprintf_p(b, sizeof b, "abc%n", &n2);
    CHECK(rm == rh && n1 == n2 && n1 == 3);
    /* Truncation contract: return is the would-be length, the
     * buffer holds a NUL-terminated prefix. */
    rm = snprintf(a, 8, "%s", "0123456789ABCDEF");
    CHECK(rm == 16);
    CHECK(strcmp(a, "0123456") == 0);
    rm = snprintf(NULL, 0, "len=%d", 12345);
    CHECK(rm == 9); /* "len=12345" */
  }
  {
    /* Audit regressions B-8 / B-10: star width and precision
     * take the same 1<<24 clamp as the literal forms (the
     * unclamped star let one conversion demand gigabytes of
     * padding, and an INT_MIN width flipped through an
     * overflowing negation), and %jn / %tn store through their
     * own widths instead of the int default. */
    char a[256];
    int rm;
    intmax_t jv;
    ptrdiff_t tv;
    rm = snprintf(NULL, 0, "%*d", 20000000, 5);
    CHECK(rm == (1 << 24));
    rm = snprintf(NULL, 0, "%*d", INT_MIN, 5);
    CHECK(rm == (1 << 24));
    rm = snprintf(NULL, 0, "%.*d", 20000000, 5);
    CHECK(rm == (1 << 24));
    rm = snprintf(NULL, 0, "%.*d", -5, 7);
    CHECK(rm == 1); /* negative star precision: omitted */
    memset(&jv, 0xAA, sizeof jv);
    rm = snprintf(a, sizeof a, "abc%jn", &jv);
    CHECK(rm == 3 && jv == 3);
    memset(&tv, 0xAA, sizeof tv);
    rm = snprintf(a, sizeof a, "abcd%tn", &tv);
    CHECK(rm == 4 && tv == 4);
  }
  CASE_END();
}

/* ================= FILE layer over the volume ================= */

static void case_file_reads(void) {
  CASE_BEGIN("FILE reads through the volume double");
  {
    FILE* f = fopen("hello.txt", "r");
    char buf[64];
    size_t n;
    CHECK(f != NULL);
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    CHECK(n == 28);
    CHECK(strcmp(buf, "Hello, volume!\nSecond line.\n") == 0);
    CHECK(feof(f) != 0);
    CHECK(fclose(f) == 0);
  }
  {
    /* getc line by line */
    FILE* f = fopen("hello.txt", "rb");
    char line[64];
    CHECK(f != NULL);
    CHECK(fgets(line, sizeof line, f) != NULL);
    CHECK(strcmp(line, "Hello, volume!\n") == 0);
    CHECK(fgets(line, sizeof line, f) != NULL);
    CHECK(strcmp(line, "Second line.\n") == 0);
    CHECK(fgets(line, sizeof line, f) == NULL);
    fclose(f);
  }
  {
    /* patterned data with seeks */
    FILE* f = fopen("sub/data.bin", "r");
    unsigned char buf[1000];
    size_t n;
    long pos;
    CHECK(f != NULL);
    n = fread(buf, 1, 1000, f);
    CHECK(n == 1000 && memcmp(buf, g_pattern, 1000) == 0);
    pos = ftell(f);
    CHECK(pos == 1000);
    CHECK(fseek(f, 5000, SEEK_SET) == 0);
    CHECK(ftell(f) == 5000);
    n = fread(buf, 1, 1000, f);
    CHECK(n == 1000 && memcmp(buf, g_pattern + 5000, 1000) == 0);
    CHECK(fseek(f, -100, SEEK_END) == 0);
    n = fread(buf, 1, 200, f);
    CHECK(n == 100 && memcmp(buf, g_pattern + 9900, 100) == 0);
    CHECK(feof(f) != 0);
    /* SEEK_CUR accounting with a partly consumed buffer */
    CHECK(fseek(f, 100, SEEK_SET) == 0);
    CHECK(fgetc(f) == g_pattern[100]);
    CHECK(ftell(f) == 101);
    CHECK(fseek(f, 9, SEEK_CUR) == 0);
    CHECK(ftell(f) == 110);
    CHECK(fgetc(f) == g_pattern[110]);
    /* ungetc roundtrip */
    CHECK(ungetc('Z', f) == 'Z');
    CHECK(fgetc(f) == 'Z');
    CHECK(fgetc(f) == g_pattern[111]);
    /* rewind */
    rewind(f);
    CHECK(ftell(f) == 0);
    CHECK(fgetc(f) == g_pattern[0]);
    fclose(f);
  }
  {
    /* element-sized reads */
    FILE* f = fopen("sub/data.bin", "r");
    unsigned char buf[10000];
    size_t n;
    CHECK(f != NULL);
    n = fread(buf, 4, 2500, f);
    CHECK(n == 2500 && memcmp(buf, g_pattern, 10000) == 0);
    fclose(f);
  }
  {
    /* Audit regression B-6: fread on an unbuffered stream read
     * nothing at all (fill_locked declines by design there and
     * fread took the 0 as EOF). Both unbuffered shapes must
     * deliver the bytes through the direct path. */
    FILE* f = fopen("sub/data.bin", "r");
    unsigned char buf[1000];
    size_t n;
    CHECK(f != NULL);
    CHECK(setvbuf(f, NULL, _IONBF, 0) == 0);
    n = fread(buf, 1, 1000, f);
    CHECK(n == 1000 && memcmp(buf, g_pattern, 1000) == 0);
    fclose(f);
    f = fopen("sub/data.bin", "r");
    CHECK(f != NULL);
    CHECK(setvbuf(f, buf, _IOFBF, 0) == 0); /* buffer size 0 */
    n = fread(buf, 1, 1000, f);
    CHECK(n == 1000 && memcmp(buf, g_pattern, 1000) == 0);
    fclose(f);
  }
  {
    /* Audit regression B-7 / E-6: a size * nmemb product that
     * wraps size_t is refused up front with ENOMEM instead of
     * transferring the wrapped amount and reporting success. */
    FILE* f = fopen("sub/data.bin", "r");
    unsigned char buf[16];
    size_t n;
    CHECK(f != NULL);
    errno = 0;
    n = fread(buf, (size_t)1 << 32, (size_t)1 << 32, f);
    CHECK(n == 0);
    CHECK(errno == ENOMEM);
    fclose(f);
  }
  CASE_END();
}

static void case_file_failsoft(void) {
  CASE_BEGIN("FILE fail-soft table (volume semantics)");
  {
    FILE* f;
    errno = 0;
    f = fopen("hello.txt", "w");
    CHECK(f == NULL);
    CHECK(errno == EROFS);
    errno = 0;
    f = fopen("hello.txt", "r+");
    CHECK(f == NULL);
    CHECK(errno == EROFS);
    errno = 0;
    f = fopen("no/such/file.txt", "r");
    CHECK(f == NULL);
    CHECK(errno == ENOENT);
    errno = 0;
    f = fopen("brand-new.txt", "w");
    CHECK(f == NULL);
    CHECK(errno == ENOSYS);
    errno = 0;
    CHECK(remove("hello.txt") == -1);
    CHECK(errno == ENOSYS);
    errno = 0;
    CHECK(rename("a", "b") == -1);
    CHECK(errno == ENOSYS);
    /* traversal out of the volume root is refused */
    errno = 0;
    f = fopen("../outside.txt", "r");
    CHECK(f == NULL);
    /* Audit regression E-4: the volume's own code surfaces
     * (EPERM for the refusal); fopen used to collapse every
     * read-mode volume failure into ENOENT. */
    CHECK(errno == EPERM);
    /* Audit regression E-3: a directory inside the volume is a
     * miss, not a readable stream; an empty path likewise. */
    errno = 0;
    f = fopen("sub", "r");
    CHECK(f == NULL);
    CHECK(errno == ENOENT);
    errno = 0;
    f = fopen("", "r");
    CHECK(f == NULL);
    CHECK(errno == ENOENT);
    /* Audit regression B-7 / E-6, write side: a wrapping
     * product is refused (the stream here is stdout, whose fd
     * the refusal must never reach). */
    errno = 0;
    CHECK(fwrite("x", (size_t)1 << 32, (size_t)1 << 32, stdout) == 0);
    CHECK(errno == ENOMEM);
    CHECK(ferror(stdout) != 0);
    clearerr(stdout);
  }
  CASE_END();
}

static void case_stdout_capture(void) {
  CASE_BEGIN("stdout/stderr routing and line buffering");
  {
    /* Redirect host fd 1: the LithoSys double writes there, so
     * our stdout lands in the capture file. */
    char cap_path[512];
    char content[512];
    int saved, saved2, capfd;
    ssize_t got;
    h_snprintf(cap_path, sizeof cap_path, "%s/capture.txt", g_root);
    saved = dup(1);
    saved2 = dup(2);
    capfd = open(cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(capfd >= 0);
    dup2(capfd, 1);
    dup2(capfd, 2);
    printf("log line %d\n", 7);
    /* Line buffering: the newline pushed the line out already,
     * the partial line must wait for fflush. */
    fputs("partial", stdout);
    {
      int probe = open(cap_path, O_RDONLY);
      char pbuf[64];
      ssize_t pn = read(probe, pbuf, sizeof pbuf - 1);
      close(probe);
      if (pn < 0) pn = 0;
      pbuf[pn] = '\0';
      CHECK(strcmp(pbuf, "log line 7\n") == 0);
    }
    fflush(stdout);
    fputs("err!", stderr); /* unbuffered: immediate */
    dup2(saved, 1);
    dup2(saved2, 2);
    close(saved);
    close(saved2);
    close(capfd);
    {
      int fd = open(cap_path, O_RDONLY);
      got = read(fd, content, sizeof content - 1);
      close(fd);
      if (got < 0) got = 0;
      content[got] = '\0';
      CHECK(strcmp(content, "log line 7\npartialerr!") == 0);
    }
  }
  CASE_END();
}

static void case_scanf(void) {
  CASE_BEGIN("scanf family vs the census shapes");
  {
    int a = 0, b = 0, c = 0;
    float x = 0, y = 0, z = 0;
    unsigned long u1 = 0, u2 = 0;
    char word[32];
    int n;
    n = sscanf("12,-34", "%d,%d", &a, &b);
    CHECK(n == 2 && a == 12 && b == -34);
    n = sscanf("640,480,16", "%d,%d,%d", &a, &b, &c);
    CHECK(n == 3 && a == 640 && b == 480 && c == 16);
    n = sscanf("1.5,-2.25,3e2", "%f,%f,%f", &x, &y, &z);
    CHECK(n == 3 && x == 1.5f && y == -2.25f && z == 300.0f);
    n = sscanf("1266 1279", "%lu %lu", &u1, &u2);
    CHECK(n == 2 && u1 == 1266 && u2 == 1279);
    n = sscanf("hello world", "%31s", word);
    CHECK(n == 1 && strcmp(word, "hello") == 0);
    n = sscanf("0x1F 010 99", "%i %i %d", &a, &b, &c);
    CHECK(n == 3 && a == 31 && b == 8 && c == 99);
    {
      unsigned int ux = 0;
      n = sscanf("deadBEEF", "%x", &ux);
      CHECK(n == 1 && ux == 0xdeadbeefu);
    }
    {
      long consumed = -1;
      n = sscanf("42 rest", "%d%ln", &a, &consumed);
      CHECK(n == 1 && a == 42 && consumed == 2);
    }
    {
      /* suppressed assignment */
      n = sscanf("10 20", "%*d %d", &a);
      CHECK(n == 1 && a == 20);
    }
    {
      /* scanset */
      char set[32];
      n = sscanf("abcXYZ", "%[a-z]", set);
      CHECK(n == 1 && strcmp(set, "abc") == 0);
      n = sscanf("abcXYZ", "%[^a-z]", set);
      CHECK(n == 0);
    }
    {
      /* char conversion: no whitespace skip, no NUL */
      char ch[3] = {0, 0, 0};
      n = sscanf(" xy", "%c", ch);
      CHECK(n == 1 && ch[0] == ' ');
      n = sscanf("xyz", "%2c", ch);
      CHECK(n == 1 && ch[0] == 'x' && ch[1] == 'y');
    }
    {
      /* matching failure and EOF */
      n = sscanf("zzz", "%d", &a);
      CHECK(n == 0);
      n = sscanf("", "%d", &a);
      CHECK(n == EOF);
      n = sscanf("7", "%d %d", &a, &b);
      CHECK(n == 1 && a == 7);
    }
    {
      /* width-limited */
      n = sscanf("123456", "%3d", &a);
      CHECK(n == 1 && a == 123);
      n = sscanf("3.14159", "%4f", &x);
      CHECK(n == 1 && x == 3.14f); /* width 4 takes "3.14" */
    }
    {
      /* fscanf through the volume on the real census shape */
      FILE* f = fopen("counts.txt", "r");
      CHECK(f != NULL);
      u1 = u2 = 0;
      n = fscanf(f, "%lu %lu", &u1, &u2);
      CHECK(n == 2 && u1 == 1266 && u2 == 1279);
      fclose(f);
    }
    {
      /* float dangling exponent: the e is not consumed */
      char rest[8];
      n = sscanf("2e!", "%f%c", &x, rest);
      CHECK(n == 2 && x == 2.0f && rest[0] == 'e');
    }
    {
      /* Audit regression B-5: inf / nan match by prefix and the
       * rest of the run goes back, so the next conversion stays
       * in sync (the old reader ate "info" whole and left the
       * 42 stranded past a consumed 'o'). */
      double dv = 0.0;
      /* "info42": %f matches "inf" and the stream resumes at
       * 'o' (consumed == 3, and a following %s sees "o42");
       * the old reader consumed the whole run (consumed == 4,
       * %s saw "42"). */
      n = sscanf("info42", "%f%n", &x, &a);
      CHECK(n == 1 && isinf((double)x) && a == 3);
      n = sscanf("info42", "%f%s", &x, word);
      CHECK(n == 2 && isinf((double)x) && strcmp(word, "o42") == 0);
      n = sscanf("infinity5", "%f%d", &x, &a);
      CHECK(n == 2 && isinf((double)x) && a == 5);
      n = sscanf("nan(9)7", "%lf%d", &dv, &a);
      CHECK(n == 2 && isnan(dv) && a == 7);
      n = sscanf("-inf!", "%f%c", &x, word);
      CHECK(n == 2 && isinf((double)x) && (double)x < 0 && word[0] == '!');
    }
    {
      /* Audit regression B-4: an over-long width literal
       * saturates at printf's 1<<24 ceiling instead of wrapping
       * through the sign into the unlimited sentinel. The
       * clamped widths below read exactly 1<<24 characters of
       * the 16 MiB-plus run; an unclamped or wrapped width
       * cannot produce that count. */
      static char big_src[(1 << 24) + 64];
      static char big_dst[(1 << 24) + 64];
      long consumed = -1;
      memset(big_src, 'a', sizeof big_src - 1);
      big_src[sizeof big_src - 1] = '\0';
      n = sscanf(big_src, "%99999999999999999999s%ln", big_dst,
                 &consumed);
      CHECK(n == 1);
      CHECK(consumed == (1 << 24));
      CHECK(strlen(big_dst) == (size_t)(1 << 24));
      consumed = -1;
      n = sscanf(big_src, "%99999999999999999999[a]%ln", big_dst,
                 &consumed);
      CHECK(n == 1);
      CHECK(consumed == (1 << 24));
    }
  }
  CASE_END();
}

/* ================= stdlib differentials ================= */

static void case_strtol_family(void) {
  CASE_BEGIN("strtol family vs host");
  {
    static const struct {
      const char* s;
      int base;
    } cases[] = {
        {"0", 10},        {"42", 10},      {"  -42", 10},
        {"+77", 10},      {"0x1F", 0},     {"0X1f", 16},
        {"010", 0},       {"0", 0},        {"0x", 0},
        {"777", 8},       {"-0x10", 0},    {"12z", 10},
        {"z", 10},        {"  ", 10},      {"2147483648", 10},
        {"9223372036854775807", 10},       {"9223372036854775808", 10},
        {"-9223372036854775808", 10},      {"-9223372036854775809", 10},
        {"99999999999999999999999999", 10},{"18446744073709551615", 10},
        {"18446744073709551616", 10},      {"-1", 10},
        {"5", 2},         {"101", 2},      {"zz", 36},
        {"0xABCDEF", 0},  {"  0x10", 0},   {"-", 10},
        {"0b101", 0},     {"08", 0},       {"09", 0},
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      char *em = NULL, *eh = NULL;
      long vm, vh;
      int erm, erh;
      errno = 0;
      vm = strtol(cases[i].s, &em, cases[i].base);
      erm = errno;
      *h_errno_loc() = 0;
      vh = h_strtol(cases[i].s, &eh, cases[i].base);
      erh = *h_errno_loc();
      if (vm != vh || (em - cases[i].s) != (eh - cases[i].s) ||
          erm != host_errno_fold(erh)) {
        case_ok = 0;
        report("  strtol(%s,%d): mine %ld (+%ld, errno %d) host %ld "
               "(+%ld, errno %d)\n",
               cases[i].s, cases[i].base, vm, (long)(em - cases[i].s),
               erm, vh, (long)(eh - cases[i].s), erh);
      }
      {
        unsigned long um, uh;
        char *fm = NULL, *fh = NULL;
        errno = 0;
        um = strtoul(cases[i].s, &fm, cases[i].base);
        erm = errno;
        *h_errno_loc() = 0;
        uh = h_strtoul(cases[i].s, &fh, cases[i].base);
        erh = *h_errno_loc();
        if (um != uh || (fm - cases[i].s) != (fh - cases[i].s) ||
            erm != host_errno_fold(erh)) {
          case_ok = 0;
          report("  strtoul(%s,%d): mine %lu (+%ld, e%d) host %lu "
                 "(+%ld, e%d)\n",
                 cases[i].s, cases[i].base, um, (long)(fm - cases[i].s),
                 erm, uh, (long)(fh - cases[i].s), erh);
        }
      }
    }
  }
  CHECK(atoi("  -123") == -123);
  CHECK(atol("987654") == 987654L);
  CHECK(atoll("-9223372036854775808") == -9223372036854775807LL - 1);
  CHECK(abs(-7) == 7 && labs(-7L) == 7L && llabs(-9LL) == 9LL);
  {
    div_t d = div(-7, 2);
    lldiv_t ld = lldiv(-7LL, 2LL);
    CHECK(d.quot == -3 && d.rem == -1);
    CHECK(ld.quot == -3 && ld.rem == -1);
  }
  CASE_END();
}

static void case_strtod(void) {
  CASE_BEGIN("strtod vs host (bit-exact + errno + endptr)");
  {
    static const char* const cases[] = {
        "0", "1", "-1.5", "3.141592653589793", "0.1", "0.5", "2.5e-1",
        "1e300", "1e-300", "1e308", "5e-324", "4.9e-324", "2.5e-324",
        "2.4703282292062327e-324", "2.4703282292062328e-324",
        "1e309", "-1e309", "1e-400", "1e-320", "123456789.123456789",
        "0.000001", "1.", ".5", "5.", "007", "1e", "1e+", "1e+5",
        "0x1.8p1", "0x1p-1074", "0x1.fffffffffffffp1023", "0x10",
        "0x1p4", "inf", "-INFINITY", "Infinity", "nan", "  2.5e2xyz",
        "1.7976931348623157e308", "1.7976931348623159e308",
        "9007199254740993", "0.30000000000000004",
        "5708990770823839207320493820740630171355185152.0",
        "3.14159265358979323846264338327950288419716939937510"
        "58209749445923078164062862089986280348253421170679"
        "82148086513282306647093844609550582231725359408128"
        "48111745028410270193852110555964462294895493038196",
        "1.0000000000000002220446049250313080847263336181640625",
        "7.038531e-26", "1234567890123456789012345678901234567890e-40",
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      char *em = NULL, *eh = NULL;
      double vm, vh;
      int erm, erh;
      errno = 0;
      vm = strtod(cases[i], &em);
      erm = errno;
      *h_errno_loc() = 0;
      vh = h_strtod(cases[i], &eh);
      erh = *h_errno_loc();
      if (!dbl_bit_eq(vm, vh) || (em - cases[i]) != (eh - cases[i]) ||
          erm != host_errno_fold(erh)) {
        case_ok = 0;
        report("  strtod(%.40s): mine %a (+%ld, e%d) host %a (+%ld, "
               "e%d)\n",
               cases[i], vm, (long)(em - cases[i]), erm, vh,
               (long)(eh - cases[i]), erh);
      }
    }
  }
  {
    /* 900 significant digits: sticky territory. */
    char big[1024];
    int i;
    double vm, vh;
    big[0] = '1';
    for (i = 1; i < 900; i++) big[i] = (char)('0' + (i % 10));
    strcpy(big + 900, "e-899");
    errno = 0;
    vm = strtod(big, NULL);
    vh = h_strtod(big, NULL);
    CHECK(dbl_bit_eq(vm, vh));
    /* A long run of 9s at a rounding boundary. */
    big[0] = '5';
    for (i = 1; i < 850; i++) big[i] = '9';
    strcpy(big + 850, "e-849");
    vm = strtod(big, NULL);
    vh = h_strtod(big, NULL);
    CHECK(dbl_bit_eq(vm, vh));
  }
  {
    /* random decimal strings */
    int k;
    for (k = 0; k < 3000; k++) {
      char s[64];
      double vm, vh;
      int ndig = 1 + (int)(rnd() % 20);
      int ep = (int)(rnd() % 640) - 320;
      int j, pos = 0;
      if (rnd() & 1) s[pos++] = '-';
      for (j = 0; j < ndig; j++) {
        if (j == 1) s[pos++] = '.';
        s[pos++] = (char)('0' + (int)(rnd() % 10));
      }
      pos += h_snprintf(s + pos, sizeof s - (size_t)pos, "e%d", ep);
      s[pos] = '\0';
      vm = strtod(s, NULL);
      vh = h_strtod(s, NULL);
      if (!dbl_bit_eq(vm, vh)) {
        case_ok = 0;
        report("  strtod random %s: mine %a host %a\n", s, vm, vh);
      }
    }
  }
  CHECK(atof("2.5") == 2.5);
  CASE_END();
}

static int cmp_int(const void* a, const void* b) {
  int x = *(const int*)a, y = *(const int*)b;
  return (x > y) - (x < y);
}
static int cmp_str(const void* a, const void* b) {
  return strcmp(*(char* const*)a, *(char* const*)b);
}

static void case_qsort_bsearch(void) {
  CASE_BEGIN("qsort vs host on random + adversarial; bsearch");
  {
    static const int sizes[] = {0, 1, 2, 3, 17, 100, 5000, 30000};
    size_t si;
    for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
      int n = sizes[si];
      int* a = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
      int* b = (int*)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
      int i, pattern;
      for (pattern = 0; pattern < 5; pattern++) {
        for (i = 0; i < n; i++) {
          switch (pattern) {
            case 0: a[i] = (int)rnd(); break;
            case 1: a[i] = i; break;             /* sorted */
            case 2: a[i] = n - i; break;         /* reversed */
            case 3: a[i] = 42; break;            /* all equal */
            default: a[i] = (int)(rnd() % 7); break; /* duplicates */
          }
        }
        memcpy(b, a, sizeof(int) * (size_t)n);
        qsort(a, (size_t)n, sizeof(int), cmp_int);
        h_qsort(b, (size_t)n, sizeof(int), cmp_int);
        CHECK(memcmp(a, b, sizeof(int) * (size_t)n) == 0);
      }
      free(a);
      free(b);
    }
  }
  {
    char* words[] = {"pear", "apple", "kiwi", "banana", "fig"};
    qsort(words, 5, sizeof words[0], cmp_str);
    CHECK(strcmp(words[0], "apple") == 0);
    CHECK(strcmp(words[4], "pear") == 0);
  }
  {
    int arr[100];
    int i, key, *hit;
    for (i = 0; i < 100; i++) arr[i] = i * 3;
    key = 42;
    hit = (int*)bsearch(&key, arr, 100, sizeof(int), cmp_int);
    CHECK(hit != NULL && *hit == 42);
    key = 43;
    hit = (int*)bsearch(&key, arr, 100, sizeof(int), cmp_int);
    CHECK(hit == NULL);
  }
  CASE_END();
}

static void case_stdlib_misc(void) {
  CASE_BEGIN("stdlib misc: rand, getenv, system, mb");
  {
    int seq1[8], seq2[8], i;
    srand(42);
    for (i = 0; i < 8; i++) seq1[i] = rand();
    srand(42);
    for (i = 0; i < 8; i++) seq2[i] = rand();
    CHECK(memcmp(seq1, seq2, sizeof seq1) == 0);
    for (i = 0; i < 8; i++) CHECK(seq1[i] >= 0 && seq1[i] <= RAND_MAX);
    srand(7);
    CHECK(rand() != seq1[0] || 1); /* different seed, no value claim */
  }
  CHECK(getenv("LL_DEFINITELY_NOT_SET") == NULL);
  __litholibc_env_inject("LL_TEST_FLAG=on");
  {
    const char* v = getenv("LL_TEST_FLAG");
    CHECK(v != NULL && strcmp(v, "on") == 0);
  }
  errno = 0;
  CHECK(system("anything") == -1);
  CHECK(errno == ENOSYS);
  CHECK(mblen("a", 1) == 1);
  CHECK(mblen("", 1) == 0);
  {
    wchar_t wc = 0;
    char mb[4];
    CHECK(mbtowc(&wc, "Z", 1) == 1 && wc == L'Z');
    CHECK(wctomb(mb, L'Q') == 1 && mb[0] == 'Q');
    errno = 0;
    CHECK(mbtowc(&wc, "\xc3\xa9", 2) == -1);
    CHECK(errno == EILSEQ);
  }
  CASE_END();
}

/* ================= math differentials =================
 * Exact families must match bit for bit. Transcendentals report
 * their worst ulp distance (and, for the trig kernels, the worst
 * absolute error: an ulp count near a result of zero measures
 * the wrong thing). The bounds asserted here are the ones
 * docs/LITHOLIBC-STDIO.md documents; if a bound fails, the doc
 * and the bound move together, on evidence. */

static math1_t m_sqrt, m_log, m_log2, m_log10, m_exp, m_sin, m_cos,
    m_tan, m_asin, m_acos, m_atan, m_floor, m_ceil, m_trunc, m_round;
static math2_t m_pow, m_atan2, m_hypot, m_fmod, m_remainder;

typedef double (*our1_t)(double);
typedef double (*our2_t)(double, double);

static double sweep_arg(int kind, int k) {
  (void)k;
  /* Deterministic argument spread per function kind. */
  double u = rnd_unit();
  switch (kind) {
    case 0: /* positive, log-spaced 1e-300..1e300 */
      return pow(10.0, -300.0 + 600.0 * u);
    case 1: /* exp domain */
      return -700.0 + 1400.0 * u;
    case 2: /* trig, moderate */
      return -1e6 + 2e6 * u;
    case 3: /* trig, small */
      return -10.0 + 20.0 * u;
    case 4: /* [-1, 1] */
      return -1.0 + 2.0 * u;
    case 5: /* atan domain, log-spaced both signs */
      return (u < 0.5 ? -1.0 : 1.0) * pow(10.0, -20.0 + 26.0 * rnd_unit());
    default: /* positive moderate */
      return 100.0 * u + 1e-9;
  }
}

static void math1_case(const char* name, int kind, int iters, our1_t ours,
                       math1_t host, long long ulp_bound, int exact) {
  CASE_BEGIN(name);
  {
    long long worst = 0;
    double worst_abs = 0.0, worst_at = 0.0;
    int k;
    for (k = 0; k < iters; k++) {
      double x = sweep_arg(kind, k);
      double a = ours(x), b = host(x);
      if (exact) {
        if (!dbl_bit_eq(a, b)) {
          case_ok = 0;
          if (worst == 0)
            report("  %s(%a): mine %a host %a\n", name, x, a, b);
          worst = 1;
        }
      } else {
        long long u = ulp_diff(a, b);
        double ae = a > b ? a - b : b - a;
        if (u > worst) {
          worst = u;
          worst_at = x;
        }
        if (ae > worst_abs) worst_abs = ae;
      }
    }
    if (!exact) {
      report("  %s: worst %lld ulp (at %a), worst abs %a\n", name, worst,
             worst_at, worst_abs);
      CHECK(worst <= ulp_bound);
    }
  }
  CASE_END();
}

static void case_math_exact(void) {
  CASE_BEGIN("math exact families (bitwise vs host)");
  {
    int k;
    int bad_sqrt = 0, bad_floor = 0, bad_fmod = 0, bad_rem = 0,
        bad_misc = 0;
    for (k = 0; k < 4000; k++) {
      double x = sweep_arg(0, k);
      double y, a, b;
      if (!dbl_bit_eq(sqrt(x), m_sqrt(x))) {
        if (bad_sqrt++ == 0)
          report("  sqrt(%a): mine %a host %a\n", x, sqrt(x),
                 m_sqrt(x));
      }
      if (!dbl_bit_eq(floor(x), m_floor(x)) ||
          !dbl_bit_eq(ceil(x), m_ceil(x)))
        bad_floor++;
      y = sweep_arg(2, k);
      if (!dbl_bit_eq(trunc(y), m_trunc(y)) ||
          !dbl_bit_eq(round(y), m_round(y)))
        bad_floor++;
      a = sweep_arg(2, k);
      b = sweep_arg(6, k) + 0.5;
      if (!dbl_bit_eq(fmod(a, b), m_fmod(a, b))) {
        if (bad_fmod++ == 0)
          report("  fmod(%a, %a): mine %a host %a\n", a, b, fmod(a, b),
                 m_fmod(a, b));
      }
      if (!dbl_bit_eq(remainder(a, b), m_remainder(a, b))) {
        if (bad_rem++ == 0)
          report("  remainder(%a, %a): mine %a host %a\n", a, b,
                 remainder(a, b), m_remainder(a, b));
      }
      if (!dbl_bit_eq(fmod(b, a), m_fmod(b, a))) bad_fmod++;
      {
        int e = 0;
        double fr = frexp(x, &e);
        double back = ldexp(fr, e);
        double ip;
        double frac;
        if (!dbl_bit_eq(back, x)) bad_misc++;
        frac = modf(y, &ip);
        if (!dbl_bit_eq(ip + frac, y)) bad_misc++;
      }
    }
    report("  exact-family mismatches: sqrt %d, rounding %d, "
           "fmod %d, remainder %d, misc %d (of 4000)\n",
           bad_sqrt, bad_floor, bad_fmod, bad_rem, bad_misc);
    CHECK(bad_sqrt == 0);
    CHECK(bad_floor == 0);
    CHECK(bad_fmod == 0);
    CHECK(bad_rem == 0);
    CHECK(bad_misc == 0);
    /* special values */
    CHECK(sqrt(0.0) == 0.0);
    CHECK(dbl_bit_eq(sqrt(5e-324), m_sqrt(5e-324)));
    CHECK(dbl_bit_eq(sqrt(1.7976931348623157e308),
                     m_sqrt(1.7976931348623157e308)));
    /* Audit regression B-3: an extreme ldexp exponent used to
     * overflow the internal be + e addition itself (undefined
     * behavior). The results saturate exactly as the host's:
     * +Inf with ERANGE upward, zero with ERANGE downward, and
     * the subnormal-input path (which adds frexp's exponent
     * internally) holds the same line. */
    {
      volatile int emax = INT_MAX, emin = INT_MIN;
      volatile double one = 1.0, denorm = 4.9e-324;
      errno = 0;
      CHECK(isinf(ldexp(one, emax)));
      CHECK(errno == ERANGE);
      errno = 0;
      CHECK(ldexp(one, emin) == 0.0);
      CHECK(errno == ERANGE);
      CHECK(isinf(ldexp(denorm, emax)));
      CHECK(ldexp(1.5, 4) == 24.0);
    }
    CHECK(fmod(-7.0, 3.0) == -1.0);
    CHECK(remainder(-7.0, 3.0) == -1.0);
    CHECK(remainder(7.0, 3.0) == 1.0);
    CHECK(remainder(5.0, 3.0) == -1.0); /* tie to even quotient 2 */
    CHECK(remainder(3.0, 4.0) == -1.0);
  }
  CASE_END();
}

static void case_math_transcendental(void) {
  math1_case("log", 0, 6000, log, m_log, 4, 0);
  math1_case("log2", 0, 6000, log2, m_log2, 4, 0);
  math1_case("log10", 0, 6000, log10, m_log10, 4, 0);
  math1_case("exp", 1, 6000, exp, m_exp, 4, 0);
  math1_case("asin", 4, 4000, asin, m_asin, 4, 0);
  math1_case("acos", 4, 4000, acos, m_acos, 4, 0);
  math1_case("atan", 5, 4000, atan, m_atan, 4, 0);
  {
    /* near-1 log density: relative accuracy at the kink */
    CASE_BEGIN("log near 1");
    {
      long long worst = 0;
      int k;
      for (k = -2000; k <= 2000; k++) {
        double x = 1.0 + k * 1e-6;
        long long u = ulp_diff(log(x), m_log(x));
        if (u > worst) worst = u;
      }
      report("  log near 1: worst %lld ulp\n", worst);
      CHECK(worst <= 4);
    }
    CASE_END();
  }
}

static void case_math_trig(void) {
  CASE_BEGIN("sin/cos/tan sweeps");
  {
    long long worst_sin = 0, worst_cos = 0, worst_tan = 0;
    double worst_sin_abs = 0.0;
    int k;
    for (k = 0; k < 8000; k++) {
      double x = sweep_arg(k % 2 ? 2 : 3, k);
      double a = sin(x), b = m_sin(x);
      long long u = ulp_diff(a, b);
      double ae = a > b ? a - b : b - a;
      if (fabs(b) > 0.01 && u > worst_sin) worst_sin = u;
      if (ae > worst_sin_abs) worst_sin_abs = ae;
      a = cos(x);
      b = m_cos(x);
      u = ulp_diff(a, b);
      if (fabs(b) > 0.01 && u > worst_cos) worst_cos = u;
      a = tan(x);
      b = m_tan(x);
      if (fabs(b) < 1e6) {
        u = ulp_diff(a, b);
        if (fabs(b) > 0.01 && u > worst_tan) worst_tan = u;
      }
    }
    report("  sin worst %lld ulp (|r|>0.01), abs %a; cos worst %lld; "
           "tan worst %lld\n",
           worst_sin, worst_sin_abs, worst_cos, worst_tan);
    CHECK(worst_sin <= 4);
    CHECK(worst_cos <= 4);
    CHECK(worst_sin_abs <= 4e-16);
    CHECK(worst_tan <= 8);
    /* exact anchors */
    CHECK(sin(0.0) == 0.0);
    CHECK(cos(0.0) == 1.0);
    CHECK(fabs(sin(3.141592653589793)) < 2e-16);
    /* Audit regression B-2: past the exact-reduction bound
     * (|x| < 2^31 * pi/2, the wasm32 production shape's limit)
     * the old code cast out of range (undefined behavior) and
     * returned finite garbage (sin(1e18) came back -3.5e22).
     * The defined fallback is NaN with EDOM, on all three. The
     * arguments ride through volatile so the compiler cannot
     * constant-fold the calls into its own libm: the library's
     * answers are what is being measured. */
    {
      volatile double huge1 = 1e18, huge2 = 1e300, huge3 = -1e18;
      volatile double past = 4.0e9, inside = 3.0e9;
      errno = 0;
      CHECK(isnan(sin(huge1)));
      CHECK(errno == EDOM);
      errno = 0;
      CHECK(isnan(cos(huge2)));
      CHECK(errno == EDOM);
      errno = 0;
      CHECK(isnan(tan(huge3)));
      CHECK(errno == EDOM);
      errno = 0;
      CHECK(isnan(sin(past))); /* just past the bound */
      CHECK(errno == EDOM);
      /* Just inside the bound the reduction is still honest. */
      CHECK(ulp_diff(sin(inside), m_sin(inside)) <= 8);
      CHECK(ulp_diff(cos(inside), m_cos(inside)) <= 8);
    }
  }
  CASE_END();
}

static void case_math_pow_atan2_hypot(void) {
  CASE_BEGIN("pow / atan2 / hypot sweeps");
  {
    long long worst_pow = 0, worst_int = 0, worst_a2 = 0, worst_hy = 0;
    int k;
    for (k = 0; k < 6000; k++) {
      double x = 0.01 + 100.0 * rnd_unit();
      double y = -20.0 + 40.0 * rnd_unit();
      long long u = ulp_diff(pow(x, y), m_pow(x, y));
      if (u > worst_pow) worst_pow = u;
      /* integral exponents through the fast path */
      {
        int iy = (int)(rnd() % 65) - 32;
        u = ulp_diff(pow(x, (double)iy), m_pow(x, (double)iy));
        if (u > worst_int) worst_int = u;
      }
      {
        double a = sweep_arg(2, k), b = sweep_arg(3, k);
        u = ulp_diff(atan2(a, b), m_atan2(a, b));
        if (u > worst_a2) worst_a2 = u;
        u = ulp_diff(hypot(a, b), m_hypot(a, b));
        if (u > worst_hy) worst_hy = u;
      }
    }
    report("  pow general worst %lld ulp; pow integral worst %lld; "
           "atan2 worst %lld; hypot worst %lld\n",
           worst_pow, worst_int, worst_a2, worst_hy);
    CHECK(worst_pow <= 32);
    CHECK(worst_int <= 4);
    CHECK(worst_a2 <= 4);
    CHECK(worst_hy <= 4);
    /* anchors and specials */
    CHECK(pow(2.0, 10.0) == 1024.0);
    CHECK(pow(-2.0, 3.0) == -8.0);
    CHECK(pow(9.0, 0.5) == 3.0 || fabs(pow(9.0, 0.5) - 3.0) < 1e-15);
    CHECK(hypot(3.0, 4.0) == 5.0);
    CHECK(atan2(0.0, -1.0) == 3.141592653589793);
    CHECK(isnan(pow(-2.0, 0.5)));
    /* Audit regression B-1: integer powering that overflowed
     * mid-computation poisoned its Dekker residuals (Inf - Inf)
     * and returned NaN for a clean signed infinity. Volatile
     * arguments, as in the trig case: the calls must reach the
     * library, not the compiler's folder. */
    {
      volatile double b1 = 1e300, b2 = 1e200, b3 = -1e300;
      volatile double two = 2.0, three = 3.0, top = 4096.0;
      volatile double mid = 1e150, tiny = 1e-300, two_b = 2.0;
      errno = 0;
      CHECK(isinf(pow(b1, two)) && pow(b1, two) > 0);
      CHECK(errno == ERANGE);
      errno = 0;
      CHECK(isinf(pow(b2, three)));
      CHECK(errno == ERANGE);
      CHECK(isinf(pow(b3, three)) && signbit(pow(b3, three)));
      errno = 0;
      CHECK(isinf(pow(two_b, top)));
      CHECK(errno == ERANGE);
      /* Below the overflow line the dd path is untouched. */
      CHECK(ulp_diff(pow(mid, two), m_pow(mid, two)) <= 4);
      CHECK(pow(tiny, two) == 0.0);
    }
  }
  CASE_END();
}

/* ================= time ================= */

static void case_time(void) {
  CASE_BEGIN("time: gmtime/mktime/strftime vs host");
  {
    static const long long epochs[] = {
        0, 1, -1, 86400, -86400, 1000000000, 1761696000,
        4102444800LL, -2208988800LL, 253402300799LL, 951782400,
        1600000000, -62167219200LL};
    size_t i;
    for (i = 0; i < sizeof epochs / sizeof epochs[0]; i++) {
      time_t t = (time_t)epochs[i];
      struct tm mine, host;
      char mb[128], hb[128];
      size_t mr, hr;
      gmtime_r(&t, &mine);
      h_gmtime_r(&t, &host);
      CHECK(mine.tm_year == host.tm_year && mine.tm_mon == host.tm_mon &&
            mine.tm_mday == host.tm_mday && mine.tm_hour == host.tm_hour &&
            mine.tm_min == host.tm_min && mine.tm_sec == host.tm_sec &&
            mine.tm_wday == host.tm_wday && mine.tm_yday == host.tm_yday);
      if (mine.tm_year >= -1800) { /* year >= 100: see above */
        mr = strftime(mb, sizeof mb, "%Y-%m-%d %H:%M:%S %a %j", &mine);
        hr = h_strftime(hb, sizeof hb, "%Y-%m-%d %H:%M:%S %a %j", &host);
        CHECK(mr == hr && strcmp(mb, hb) == 0);
        mr = strftime(mb, sizeof mb, "%F %T %p %I", &mine);
        hr = h_strftime(hb, sizeof hb, "%F %T %p %I", &host);
        CHECK(mr == hr && strcmp(mb, hb) == 0);
      }
    }
    /* mktime round trips */
    for (i = 0; i < sizeof epochs / sizeof epochs[0]; i++) {
      time_t t = (time_t)epochs[i];
      struct tm br;
      time_t back;
      localtime_r(&t, &br);
      back = mktime(&br);
      CHECK(back == t);
    }
    /* mktime normalization: month 13, day 0 */
    {
      struct tm br;
      time_t t;
      memset(&br, 0, sizeof br);
      br.tm_year = 126; /* 2026 */
      br.tm_mon = 12;   /* "January 2027" */
      br.tm_mday = 1;
      t = mktime(&br);
      CHECK(br.tm_year == 127 && br.tm_mon == 0 && br.tm_mday == 1);
      CHECK(t == (time_t)1798761600); /* 2027-01-01 00:00:00 UTC */
    }
    {
      time_t now = time(NULL);
      CHECK(now > (time_t)1700000000);
      CHECK(difftime((time_t)10, (time_t)4) == 6.0);
      CHECK(clock() >= 0);
    }
    {
      /* Audit regression B-9: negative tm_mon / tm_wday in a
       * directly assigned broken-down time indexed before the
       * name tables (C's % keeps the sign; ASan showed a global
       * buffer overflow read). Normalized: -1 is the last entry
       * of each cycle. */
      struct tm br;
      char mb[128];
      memset(&br, 0, sizeof br);
      br.tm_year = 126;
      br.tm_mday = 15;
      br.tm_mon = -1;
      br.tm_wday = -1;
      strftime(mb, sizeof mb, "%B %A", &br);
      CHECK(strcmp(mb, "December Saturday") == 0);
    }
  }
  CASE_END();
}

/* ================= audit regressions: host seams and exit ================= */

static void case_audit_host_seams(void) {
  CASE_BEGIN("audit E-2/E-5: fd_write total error, EINTR retry");
  {
    /* E-2 through the double directly: [4-byte iov][4-byte iov],
     * the first write capped at 3 bytes, the second write
     * failing. The frozen semantics: success with the true
     * count, and the second iov is never attempted. The old
     * double broke only its inner loop on the error and went on
     * to write the next iov in full. */
    char path[512];
    int fd, back;
    lithosys_iov_t iovs[2];
    uint32_t written;
    lithosys_err_t err;
    char content[16];
    ssize_t got;
    h_snprintf(path, sizeof path, "%s/stage.bin", g_root);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0);
    iovs[0].buf = (void*)"abcd";
    iovs[0].len = 4;
    iovs[1].buf = (void*)"WXYZ";
    iovs[1].len = 4;
    written = 999;
    lithosys_host_double_write_script(1, 3, 2, 0);
    err = lithosys_fd_write((uint32_t)fd, iovs, 2, &written);
    CHECK(err == 0);
    CHECK(written == 3);
    CHECK(lithosys_host_double_write_calls() == 2);
    close(fd);
    back = open(path, O_RDONLY);
    got = read(back, content, sizeof content);
    close(back);
    CHECK(got == 3 && memcmp(content, "abc", 3) == 0);
    /* A write() that returns 0 is an error, not a spin (the
     * old double looped on it forever). */
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0);
    written = 999;
    lithosys_host_double_write_script(0, 0, 0, 1);
    err = lithosys_fd_write((uint32_t)fd, iovs, 1, &written);
    CHECK(err == LITHOSYS_EIO);
    CHECK(written == 0);
    CHECK(lithosys_host_double_write_calls() == 1);
    close(fd);
  }
  {
    /* E-5, write side: an EINTR out of the host mid-flush is
     * retried by the stdio layer; the bytes land and the stream
     * keeps a clean error state. Pre-fix the flush failed and
     * ferror was set. stdout is redirected to a capture file,
     * the case_stdout_capture pattern. */
    char cap_path[512];
    int saved, capfd, back;
    char content[64];
    ssize_t got;
    h_snprintf(cap_path, sizeof cap_path, "%s/eintr.txt", g_root);
    fflush(stdout); /* nothing pending may leak into the capture */
    saved = dup(1);
    capfd = open(cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(capfd >= 0);
    dup2(capfd, 1);
    close(capfd);
    lithosys_host_double_arm_eintr_write();
    CHECK(fprintf(stdout, "eintr-ok\n") >= 0);
    CHECK(fflush(stdout) == 0);
    CHECK(ferror(stdout) == 0);
    dup2(saved, 1);
    close(saved);
    back = open(cap_path, O_RDONLY);
    got = read(back, content, sizeof content - 1);
    close(back);
    if (got < 0) got = 0;
    content[got] = '\0';
    CHECK(strcmp(content, "eintr-ok\n") == 0);
  }
  {
    /* E-5, read side: an EINTR on the fill is retried and the
     * read completes with the file's real bytes. */
    FILE* f = fopen("hello.txt", "r");
    char buf[64];
    size_t n;
    CHECK(f != NULL);
    lithosys_host_double_arm_eintr_read();
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    CHECK(n == 28);
    CHECK(strcmp(buf, "Hello, volume!\nSecond line.\n") == 0);
    CHECK(ferror(f) == 0);
    fclose(f);
  }
  CASE_END();
}

static int g_audit_pipe = -1;
static void audit_tls_dtor(void* arg) {
  char b = 'T';
  ssize_t w;
  (void)arg;
  if (g_audit_pipe < 0) return;
  w = write(g_audit_pipe, &b, 1);
  (void)w;
}

static void case_audit_exit_tls(void) {
  CASE_BEGIN("audit E-1: exit() runs main-thread TLS destructors");
  {
    int fds[2];
    int status = 0;
    pid_t pid;
    char byte = 0;
    ssize_t n;
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
      /* Child: register a TLS destructor on the main thread,
       * then leave through the library's exit(). Pre-fix the
       * destructor never ran and the pipe stayed empty. */
      close(fds[0]);
      g_audit_pipe = fds[1];
      if (lw_thread_atexit(audit_tls_dtor, NULL) != LITHOSYS_OK) {
        _exit(4);
      }
      exit(7);
      _exit(3); /* exit() never returns */
    }
    CHECK(pid > 0);
    close(fds[1]);
    n = read(fds[0], &byte, 1);
    close(fds[0]);
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(n == 1 && byte == 'T');
  }
  CASE_END();
}

/* Review L6: an over-long numeral is a matching failure, and an
 * inf/nan near-miss reads no further than the word it could be. */
static void case_scanf_l6(void) {
  CASE_BEGIN("review L6: scanf long numerals and inf/nan pushback");
  {
    static char big[1400];
    double d = 7.0;
    int n, k = 0;
    char rest[8];
    memset(big, '1', 1300);
    big[1300] = 'e';
    big[1301] = '-';
    big[1302] = '9';
    big[1303] = '\0';
    n = sscanf(big, "%lf", &d);
    CHECK(n == 0);
    CHECK(d == 7.0);
    n = sscanf("info42", "%lf%2s%d", &d, rest, &k);
    CHECK(n == 3 && d > 1e308 && strcmp(rest, "o4") == 0 && k == 2);
    n = sscanf("infinity5", "%lf%d", &d, &k);
    CHECK(n == 2 && d > 1e308 && k == 5);
    n = sscanf("nanx", "%lf%s", &d, rest);
    CHECK(n == 2 && d != d && strcmp(rest, "x") == 0);
  }
  CASE_END();
}

/* Review L3: a count past INT_MAX is EOVERFLOW, not a wrapped int. */
static void case_printf_overflow(void) {
  CASE_BEGIN("review L3: printf total past INT_MAX is EOVERFLOW");
  {
    /* Widths clamp at 1 << 24, so 128 maximal fields total exactly
     * 2^31 (one past INT_MAX) and 127 stay below it. Count-only
     * sink: nothing is stored. */
#define L3_A8 1, 1, 1, 1, 1, 1, 1, 1
#define L3_A64 L3_A8, L3_A8, L3_A8, L3_A8, L3_A8, L3_A8, L3_A8, L3_A8
    static char fmt[128 * 11 + 1];
    int r, i;
    fmt[0] = '\0';
    for (i = 0; i < 128; i++) strcat(fmt, "%16777216d");
    errno = 0;
    r = snprintf(NULL, 0, fmt, L3_A64, L3_A64);
    CHECK(r == -1);
    CHECK(errno == EOVERFLOW);
    fmt[127 * 10] = '\0';
    r = snprintf(NULL, 0, fmt, L3_A64, L3_A64);
    CHECK(r == 127 * 16777216);
#undef L3_A64
#undef L3_A8
  }
  CASE_END();
}

/* ================= main ================= */

int main(void) {
  /* The heap goes up BEFORE dlopen: this binary interposes the
   * process malloc (LithoLibC over WaferHeap), and the loader's
   * own allocation for dlopen would otherwise find no heap and
   * fail with an out-of-memory error before any test runs. */
  setup_heap();
  g_libc = dlopen("libc.so.6", RTLD_NOW | RTLD_GLOBAL);
  g_libm = dlopen("libm.so.6", RTLD_NOW);
  if (g_libc == NULL || g_libm == NULL) {
    const char* e = dlerror();
    if (e) {
      ssize_t w = write(2, e, strlen(e));
      (void)w;
    }
    return 2;
  }
  h_vsnprintf = (int (*)(char*, size_t, const char*, va_list))HOST_FN(
      vsnprintf);
  h_snprintf = (int (*)(char*, size_t, const char*, ...))HOST_FN(snprintf);
  h_snprintf_p = (snprintf_t)HOST_FN(snprintf);
  h_strtol = (strtol_t)HOST_FN(strtol);
  h_strtoll = (strtoll_t)HOST_FN(strtoll);
  (void)h_strtoll;
  h_strtoul = (strtoul_t)HOST_FN(strtoul);
  h_strtoull = (strtoull_t)HOST_FN(strtoull);
  (void)h_strtoull;
  h_strtod = (strtod_t)HOST_FN(strtod);
  /* Host functions report errno in glibc's TLS slot, not in
   * __litholibc_errno_location: host errno comparisons must go
   * through glibc's own __errno_location. */
  h_errno_loc = (errno_loc_t)HOST_FN(__errno_location);
  h_qsort = (qsort_t)HOST_FN(qsort);
  h_gmtime_r = (gmtime_r_t)HOST_FN(gmtime_r);
  h_strftime = (strftime_t)HOST_FN(strftime);
  h_fopen = (hfopen_t)HOST_FN(fopen);
  h_fwrite = (hfwrite_t)HOST_FN(fwrite);
  h_fclose = (hfclose_t)HOST_FN(fclose);
  m_sqrt = (math1_t)HOSTM_FN(sqrt);
  m_log = (math1_t)HOSTM_FN(log);
  m_log2 = (math1_t)HOSTM_FN(log2);
  m_log10 = (math1_t)HOSTM_FN(log10);
  m_exp = (math1_t)HOSTM_FN(exp);
  m_sin = (math1_t)HOSTM_FN(sin);
  m_cos = (math1_t)HOSTM_FN(cos);
  m_tan = (math1_t)HOSTM_FN(tan);
  m_asin = (math1_t)HOSTM_FN(asin);
  m_acos = (math1_t)HOSTM_FN(acos);
  m_atan = (math1_t)HOSTM_FN(atan);
  m_floor = (math1_t)HOSTM_FN(floor);
  m_ceil = (math1_t)HOSTM_FN(ceil);
  m_trunc = (math1_t)HOSTM_FN(trunc);
  m_round = (math1_t)HOSTM_FN(round);
  m_pow = (math2_t)HOSTM_FN(pow);
  m_atan2 = (math2_t)HOSTM_FN(atan2);
  m_hypot = (math2_t)HOSTM_FN(hypot);
  m_fmod = (math2_t)HOSTM_FN(fmod);
  m_remainder = (math2_t)HOSTM_FN(remainder);
  if (!h_vsnprintf || !h_snprintf || !m_sqrt || !h_strtod) {
    report("host reference resolution failed\n");
    return 2;
  }

  make_fixtures();

  case_printf_integers();
  case_printf_floats();
  case_printf_mixed();
  case_file_reads();
  case_file_failsoft();
  case_stdout_capture();
  case_scanf();
  case_strtol_family();
  case_strtod();
  case_qsort_bsearch();
  case_stdlib_misc();
  case_math_exact();
  case_math_transcendental();
  case_math_trig();
  case_math_pow_atan2_hypot();
  case_time();
  case_audit_host_seams();
  case_audit_exit_tls();
  case_printf_overflow();
  case_scanf_l6();

  if (g_failures == 0) {
    report("ALL GROUPS PASS\n");
    return 0;
  }
  report("%d GROUP(S) FAILED\n", g_failures);
  return 1;
}
