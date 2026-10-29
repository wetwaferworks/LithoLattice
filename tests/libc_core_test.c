/* libc_core_test.c ... host functional test for the LithoLibC core.
 *
 * Native build and run (this file is a test, not part of the
 * library):
 *
 *   gcc -std=c99 -Wall -Wextra -I include tests/libc_core_test.c \
 *       src/libc/memory.c src/libc/string.c src/libc/strings.c \
 *       src/libc/ctype.c src/libc/malloc.c src/libc/errno.c \
 *       src/libc/strerror.c src/waferheap.c \
 *       -o /tmp/libc_core_test -ldl && /tmp/libc_core_test
 *
 * Method: the string, memory and ctype functions are checked
 * differentially against the host C library. The references are
 * fetched with dlsym on an explicit libc handle, because this
 * binary also links LithoLibC's own definitions of the same names
 * (direct calls from this file bind to LithoLibC, which is what is
 * being tested). Inputs are randomized with a fixed-seed PRNG, so a
 * failure reproduces exactly. The malloc family is exercised
 * through WaferHeap over a static arena whose grow callback always
 * refuses, which is what makes exhaustion and ENOMEM testable.
 * Prints PASS or FAIL per group and a final verdict.
 */

#include <stdio.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "waferheap.h"

static int g_failures = 0;

#define CASE_BEGIN(name) \
  const char* case_name = (name); \
  int case_ok = 1

#define CHECK(cond)                                    \
  do {                                                 \
    if (!(cond)) {                                     \
      case_ok = 0;                                     \
      printf("  check failed: %s (line %d)\n", #cond, \
             __LINE__);                                \
    }                                                  \
  } while (0)

#define CASE_END()                                   \
  do {                                               \
    printf("%s: %s\n", case_ok ? "PASS" : "FAIL",    \
           case_name);                               \
    if (!case_ok) ++g_failures;                      \
  } while (0)

/* ---- deterministic PRNG (xorshift32, fixed seed) ---- */
static uint32_t g_rng = 0x2b992adu;
static uint32_t rnd(void) {
  uint32_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng = x;
  return x;
}
static size_t rnd_below(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static int sign3(int v) { return (v > 0) - (v < 0); }

/* Identity at runtime; keeps the compiler from statically proving
 * the provenance of pointers a case deliberately misuses. */
static void* hide_ptr(void* p) { return p; }

/* ---- host references, fetched once ---- */
typedef void* (*memcpy_t)(void*, const void*, size_t);
typedef void* (*memmove_t)(void*, const void*, size_t);
typedef void* (*memset_t)(void*, int, size_t);
typedef int (*memcmp_t)(const void*, const void*, size_t);
typedef void* (*memchr_t)(const void*, int, size_t);
typedef size_t (*strlen_t)(const char*);
typedef size_t (*strnlen_t)(const char*, size_t);
typedef char* (*strcpy_t)(char*, const char*);
typedef char* (*strncpy_t)(char*, const char*, size_t);
typedef char* (*strcat_t)(char*, const char*);
typedef char* (*strncat_t)(char*, const char*, size_t);
typedef int (*strcmp_t)(const char*, const char*);
typedef int (*strncmp_t)(const char*, const char*, size_t);
typedef char* (*strchr_t)(const char*, int);
typedef strchr_t strrchr_t; /* same shape, distinct host symbol */
typedef char* (*strstr_t)(const char*, const char*);
typedef size_t (*strspn_t)(const char*, const char*);
typedef strspn_t strcspn_t; /* same shape, distinct host symbol */
typedef char* (*strpbrk_t)(const char*, const char*);
typedef char* (*strtok_r_t)(char*, const char*, char**);
typedef size_t (*strxfrm_t)(char*, const char*, size_t);
typedef int (*strcasecmp_t)(const char*, const char*);
typedef int (*strncasecmp_t)(const char*, const char*, size_t);
typedef int (*ctype_t)(int);
typedef ctype_t isalnum_t, isalpha_t, isblank_t, iscntrl_t, isdigit_t,
    isgraph_t, islower_t, isprint_t, ispunct_t, isspace_t, isupper_t,
    isxdigit_t, tolower_t, toupper_t;

static void* g_libc;
#define HOST_FN(name) \
  (name##_t) dlsym(g_libc, #name)

static memcpy_t h_memcpy;   static memmove_t h_memmove;
static memset_t h_memset;   static memcmp_t h_memcmp;
static memchr_t h_memchr;   static strlen_t h_strlen;
static strnlen_t h_strnlen; static strcpy_t h_strcpy;
static strncpy_t h_strncpy; static strcat_t h_strcat;
static strncat_t h_strncat; static strcmp_t h_strcmp;
static strncmp_t h_strncmp; static strchr_t h_strchr;
static strchr_t h_strrchr;  static strstr_t h_strstr;
static strspn_t h_strspn;   static strspn_t h_strcspn;
static strpbrk_t h_strpbrk; static strtok_r_t h_strtok_r;
static strcasecmp_t h_strcasecmp; static strncasecmp_t h_strncasecmp;
static strxfrm_t h_strxfrm;
static ctype_t h_isalnum, h_isalpha, h_isblank, h_iscntrl, h_isdigit,
    h_isgraph, h_islower, h_isprint, h_ispunct, h_isspace, h_isupper,
    h_isxdigit, h_tolower, h_toupper;

static int load_host_refs(void) {
  g_libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_LOCAL);
  if (g_libc == NULL) g_libc = dlopen("libc.so", RTLD_LAZY | RTLD_LOCAL);
  if (g_libc == NULL) return 0;
  h_memcpy = HOST_FN(memcpy);   h_memmove = HOST_FN(memmove);
  h_memset = HOST_FN(memset);   h_memcmp = HOST_FN(memcmp);
  h_memchr = HOST_FN(memchr);   h_strlen = HOST_FN(strlen);
  h_strnlen = HOST_FN(strnlen); h_strcpy = HOST_FN(strcpy);
  h_strncpy = HOST_FN(strncpy); h_strcat = HOST_FN(strcat);
  h_strncat = HOST_FN(strncat); h_strcmp = HOST_FN(strcmp);
  h_strncmp = HOST_FN(strncmp); h_strchr = HOST_FN(strchr);
  h_strrchr = HOST_FN(strrchr); h_strstr = HOST_FN(strstr);
  h_strspn = HOST_FN(strspn);   h_strcspn = HOST_FN(strcspn);
  h_strpbrk = HOST_FN(strpbrk); h_strtok_r = HOST_FN(strtok_r);
  h_strcasecmp = HOST_FN(strcasecmp);
  h_strncasecmp = HOST_FN(strncasecmp);
  h_strxfrm = HOST_FN(strxfrm);
  h_isalnum = HOST_FN(isalnum); h_isalpha = HOST_FN(isalpha);
  h_isblank = HOST_FN(isblank); h_iscntrl = HOST_FN(iscntrl);
  h_isdigit = HOST_FN(isdigit); h_isgraph = HOST_FN(isgraph);
  h_islower = HOST_FN(islower); h_isprint = HOST_FN(isprint);
  h_ispunct = HOST_FN(ispunct); h_isspace = HOST_FN(isspace);
  h_isupper = HOST_FN(isupper); h_isxdigit = HOST_FN(isxdigit);
  h_tolower = HOST_FN(tolower); h_toupper = HOST_FN(toupper);
  return h_memcpy && h_memmove && h_memset && h_memcmp && h_memchr &&
         h_strlen && h_strcpy && h_strcmp && h_strchr && h_strstr &&
         h_isalpha && h_tolower && h_strtok_r;
}

/* ---- WaferHeap default heap over a fixed arena, grow refuses ---- */
static unsigned char g_arena[256 * 1024];
static void* grow_refused(wh_heap_t* heap, size_t min_extra, size_t* out,
                          void* user) {
  (void)heap; (void)min_extra; (void)out; (void)user;
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

/* ================= cases ================= */

static void case_malloc_family(void) {
  CASE_BEGIN("malloc family over WaferHeap (fixed arena)");
  CHECK(wh_default_heap() != NULL);
  {
    void* p0 = malloc(0);
    CHECK(p0 != NULL);
    free(p0);
  }
  {
    unsigned char* p = (unsigned char*)malloc(100);
    size_t i;
    CHECK(p != NULL);
    CHECK(malloc_usable_size(p) >= 100);
    for (i = 0; i < 100; ++i) p[i] = (unsigned char)(i ^ 0x5a);
    p = (unsigned char*)realloc(p, 5000); /* grow */
    CHECK(p != NULL);
    for (i = 0; i < 100; ++i) CHECK(p[i] == (unsigned char)(i ^ 0x5a));
    p = (unsigned char*)realloc(p, 40); /* shrink */
    CHECK(p != NULL);
    for (i = 0; i < 40; ++i) CHECK(p[i] == (unsigned char)(i ^ 0x5a));
    free(p);
    free(NULL); /* no-op */
  }
  {
    unsigned char* z = (unsigned char*)calloc(64, 4);
    size_t i;
    int zeroed = 1;
    CHECK(z != NULL);
    for (i = 0; i < 256; ++i) zeroed = zeroed && (z[i] == 0);
    CHECK(zeroed);
    free(z);
  }
  {
    static const size_t aligns[] = {16, 32, 64, 128, 256};
    size_t i;
    for (i = 0; i < sizeof aligns / sizeof aligns[0]; ++i) {
      void* p = aligned_alloc(aligns[i], 96);
      CHECK(p != NULL);
      CHECK(((uintptr_t)p % aligns[i]) == 0);
      free(p);
    }
  }
  {
    void* p = (void*)(uintptr_t)0x1234; /* sentinel, must survive */
    CHECK(posix_memalign(&p, 3, 64) == EINVAL);
    CHECK(p == (void*)(uintptr_t)0x1234);
    CHECK(posix_memalign(&p, 64, 128) == 0);
    CHECK(p != NULL && ((uintptr_t)p % 64) == 0);
    free(p);
  }
  {
    /* A foreign pointer into the middle of a non-heap buffer: the
     * allocator may read the word in front of it while deciding it
     * is foreign, so the buffer gives that read somewhere valid to
     * land. The free must still be a no-op. */
    static unsigned char foreign[256];
    errno = 0;
    CHECK(aligned_alloc(24, 64) == NULL); /* not a power of two */
    CHECK(errno == EINVAL);
    free(hide_ptr(&foreign[128]));
    {
      void* p = malloc(32);
      CHECK(p != NULL);
      free(p);
    }
  }
  {
    errno = 4242; /* a successful call must not touch errno */
    {
      void* p = malloc(64);
      CHECK(p != NULL);
      CHECK(errno == 4242);
      free(p);
    }
  }
  {
    void* blocks[128];
    size_t count = 0;
    size_t i;
    void* p;
    errno = 0;
    while (count < 128 && (p = malloc(16384)) != NULL) {
      blocks[count++] = p;
    }
    CHECK(count > 0 && count < 128); /* the arena really ran out */
    CHECK(errno == ENOMEM);
    for (i = 0; i < count; ++i) free(blocks[i]);
    p = malloc(1024); /* and the heap serves again */
    CHECK(p != NULL);
    free(p);
  }
  {
    volatile size_t big = ((size_t)-1) / 2 + 1;
    errno = 0;
    CHECK(calloc(big, 2) == NULL); /* count * size overflows */
    CHECK(errno == ENOMEM);
  }
  CASE_END();
}

static void case_strdup(void) {
  CASE_BEGIN("strdup / strndup");
  {
    char src[] = "lithoroot";
    char* copy = strdup(src);
    CHECK(copy != NULL);
    CHECK(copy != src);
    CHECK(strcmp(copy, src) == 0);
    copy[0] = 'L';
    CHECK(src[0] == 'l'); /* independent storage */
    free(copy);
  }
  {
    char* t = strndup("waferheap", 5);
    CHECK(t != NULL && strcmp(t, "wafer") == 0);
    free(t);
    t = strndup("hi", 100);
    CHECK(t != NULL && strcmp(t, "hi") == 0);
    free(t);
    t = strndup("hi", 0);
    CHECK(t != NULL && t[0] == '\0');
    free(t);
    t = strdup("");
    CHECK(t != NULL && t[0] == '\0');
    free(t);
  }
  CASE_END();
}

#define ARENA 1024

static void case_memory(void) {
  CASE_BEGIN("memory functions, differential vs host");
  static unsigned char a[ARENA], b[ARENA], s[ARENA];
  int iter;
  for (iter = 0; iter < 400; ++iter) {
    const size_t n = rnd_below(601);
    const size_t doff = rnd_below(8), soff = rnd_below(8);
    size_t i;
    int c;
    for (i = 0; i < ARENA; ++i) s[i] = (unsigned char)rnd();
    /* memcpy: run ours into a, host into b, compare everything
     * (the untouched bytes prove nothing overran) */
    for (i = 0; i < ARENA; ++i) a[i] = b[i] = 0xa5;
    CHECK(memcpy(a + doff, s + soff, n) == a + doff);
    CHECK(h_memcpy(b + doff, s + soff, n) == b + doff);
    CHECK(memcmp(a, b, ARENA) == 0);
    /* memset, with a fill value wider than a byte on purpose */
    c = (int)(rnd() & 0x1ff);
    for (i = 0; i < ARENA; ++i) a[i] = b[i] = 0x3c;
    CHECK(memset(a + doff, c, n) == a + doff);
    CHECK(h_memset(b + doff, c, n) == b + doff);
    CHECK(memcmp(a, b, ARENA) == 0);
    /* memcmp: sign must agree with the host's */
    for (i = 0; i < ARENA; ++i) b[i] = a[i] = s[i];
    if (n > 0 && (rnd() & 1)) b[rnd_below(n)] ^= 0xff;
    CHECK(sign3(memcmp(a, b, n)) == sign3(h_memcmp(a, b, n)));
    /* memchr: present and absent targets */
    c = (rnd() & 1) ? s[rnd_below(ARENA)] : 0x100 + (int)rnd_below(100);
    {
      const void* ours = memchr(a, c, n);
      const void* host = h_memchr(a, c, n);
      CHECK((ours == NULL) == (host == NULL));
      if (ours && host) CHECK((const char*)ours - (const char*)a ==
                              (const char*)host - (const char*)a);
    }
  }
  CASE_END();
}

static void case_memmove(void) {
  CASE_BEGIN("memmove, overlapping ranges vs host");
  static unsigned char a[512], b[512];
  int iter;
  for (iter = 0; iter < 600; ++iter) {
    const size_t doff = rnd_below(200);
    const size_t soff = rnd_below(200);
    const size_t n = rnd_below(200);
    size_t i;
    for (i = 0; i < 512; ++i) a[i] = b[i] = (unsigned char)(i * 7 + 3);
    CHECK(memmove(a + doff, a + soff, n) == a + doff);
    CHECK(h_memmove(b + doff, b + soff, n) == b + doff);
    CHECK(memcmp(a, b, 512) == 0);
  }
  CASE_END();
}

static void fill_random_string(char* buf, size_t cap) {
  static const char alphabet[] = "abcdeABCDE 019.,_-zZ";
  const size_t len = cap ? rnd_below(cap) : 0;
  size_t i;
  for (i = 0; i < len; ++i)
    buf[i] = alphabet[rnd_below(sizeof alphabet - 1)];
  buf[len] = '\0';
}

static void case_string(void) {
  CASE_BEGIN("string functions, differential vs host");
  static char x[256], y[256], dx[512], dy[512];
  int iter;
  for (iter = 0; iter < 400; ++iter) {
    size_t maxlen;
    int c;
    fill_random_string(x, 120);
    memcpy(y, x, strlen(x) + 1);
    CHECK(strlen(x) == h_strlen(x));
    maxlen = rnd_below(200);
    CHECK(strnlen(x, maxlen) == h_strnlen(x, maxlen));
    /* comparisons against a twin and against a variant */
    fill_random_string(dy, 120);
    CHECK(sign3(strcmp(x, dy)) == sign3(h_strcmp(x, dy)));
    CHECK(sign3(strncmp(x, dy, maxlen)) ==
          sign3(h_strncmp(x, dy, maxlen)));
    CHECK(sign3(strcasecmp(x, dy)) == sign3(h_strcasecmp(x, dy)));
    CHECK(sign3(strncasecmp(x, dy, maxlen)) ==
          sign3(h_strncasecmp(x, dy, maxlen)));
    CHECK(sign3(strcoll(x, dy)) == sign3(h_strcmp(x, dy)));
    /* strchr / strrchr, including the terminator and absences */
    c = (rnd() & 1) ? "abcXYZ 0"[rnd_below(8)] : '~';
    {
      const char* o1 = strchr(x, c);
      const char* o2 = h_strchr(x, c);
      const char* r1 = strrchr(x, c);
      const char* r2 = h_strrchr(x, c);
      CHECK((o1 == NULL) == (o2 == NULL));
      CHECK((r1 == NULL) == (r2 == NULL));
      if (o1 && o2) CHECK(o1 - x == o2 - x);
      if (r1 && r2) CHECK(r1 - x == r2 - x);
      CHECK(strchr(x, '\0') == x + strlen(x));
    }
    /* spans and break search */
    {
      static const char* sets[] = {"abc", "aX 0", "", "zZ9.,"};
      const char* set = sets[rnd_below(4)];
      CHECK(strspn(x, set) == h_strspn(x, set));
      CHECK(strcspn(x, set) == h_strcspn(x, set));
      {
        const char* p1 = strpbrk(x, set);
        const char* p2 = h_strpbrk(x, set);
        CHECK((p1 == NULL) == (p2 == NULL));
        if (p1 && p2) CHECK(p1 - x == p2 - x);
      }
    }
    /* strstr: substring, random, and empty needles */
    {
      char needle[64];
      const int kind = (int)rnd_below(3);
      if (kind == 0 && strlen(x) > 0) {
        const size_t st = rnd_below(strlen(x));
        size_t room = strlen(x) - st;
        size_t ln;
        if (room > 60) room = 60; /* the needle buffer is 64 */
        ln = rnd_below(room + 1);
        memcpy(needle, x + st, ln);
        needle[ln] = '\0';
      } else if (kind == 1) {
        needle[0] = '\0';
      } else {
        fill_random_string(needle, 10);
      }
      {
        const char* p1 = strstr(x, needle);
        const char* p2 = h_strstr(x, needle);
        CHECK((p1 == NULL) == (p2 == NULL));
        if (p1 && p2) CHECK(p1 - x == p2 - x);
      }
    }
    /* strcpy / strcat / strncat on twin destinations */
    memcpy(dx, x, strlen(x) + 1);
    memcpy(dy, x, strlen(x) + 1);
    CHECK(strcat(dx, "++") == dx);
    CHECK(h_strcat(dy, "++") == dy);
    CHECK(strcmp(dx, dy) == 0);
    {
      const size_t take = rnd_below(20);
      memcpy(dx, x, strlen(x) + 1);
      memcpy(dy, x, strlen(x) + 1);
      CHECK(strncat(dx, "abcdefghij", take) == dx);
      CHECK(h_strncat(dy, "abcdefghij", take) == dy);
      CHECK(strcmp(dx, dy) == 0);
    }
    /* strncpy: full destination compare proves the zero padding */
    {
      const size_t n = rnd_below(140);
      memset(dx, 0x77, 200);
      memset(dy, 0x77, 200);
      CHECK(strncpy(dx, x, n) == dx);
      CHECK(h_strncpy(dy, x, n) == dy);
      CHECK(memcmp(dx, dy, 200) == 0);
    }
    /* strxfrm: C locale, so it mirrors the host's C locale too */
    {
      char tx[128], ty[128];
      const size_t n = rnd_below(140);
      const size_t r1 = strxfrm(n ? tx : NULL, x, n);
      const size_t r2 = h_strxfrm(n ? ty : NULL, x, n);
      CHECK(r1 == r2);
      /* Only when the result fit (r < n) are the destination
       * contents specified; a truncated strxfrm result is not
       * required to be terminated, and the host's is not. Our own
       * stronger guarantee (always terminated within n) is checked
       * separately. */
      if (n && r1 < n) CHECK(strcmp(tx, ty) == 0);
      if (n) CHECK(memchr(tx, '\0', n) != NULL);
    }
  }
  CASE_END();
}

static void case_strtok(void) {
  CASE_BEGIN("strtok_r sequence vs host; strtok basics");
  static const char* texts[] = {
    "  one,,two  three,,four ", ",,,", "single", "",
    "a.b-c.d--e", "  lead and trail  ",
  };
  static const char* delims[] = {" ,", ",", ".", " -"};
  size_t t, d;
  for (t = 0; t < sizeof texts / sizeof texts[0]; ++t) {
    for (d = 0; d < sizeof delims / sizeof delims[0]; ++d) {
      static char a[128], b[128];
      char* save_a = NULL;
      char* save_b = NULL;
      char* first_a;
      char* p;
      memcpy(a, texts[t], strlen(texts[t]) + 1);
      memcpy(b, texts[t], strlen(texts[t]) + 1);
      first_a = strtok_r(a, delims[d], &save_a);
      p = first_a;
      {
        char* q = h_strtok_r(b, delims[d], &save_b);
        for (;;) {
          CHECK((p == NULL) == (q == NULL));
          if (p == NULL || q == NULL) break;
          CHECK(strcmp(p, q) == 0);
          p = strtok_r(NULL, delims[d], &save_a);
          q = h_strtok_r(NULL, delims[d], &save_b);
        }
      }
      if (t == 0 && d == 0) {
        /* strtok (the static-state form) walks the same tokens */
        static char c1[] = "  one,,two  three,,four ";
        CHECK(strcmp(strtok(c1, " ,"), "one") == 0);
        CHECK(strcmp(strtok(NULL, " ,"), "two") == 0);
        CHECK(strcmp(strtok(NULL, " ,"), "three") == 0);
        CHECK(strcmp(strtok(NULL, " ,"), "four") == 0);
        CHECK(strtok(NULL, " ,") == NULL);
      }
    }
  }
  CASE_END();
}

static void case_ctype(void) {
  CASE_BEGIN("ctype, full -1..255 range vs host C locale");
  int c;
  for (c = -1; c <= 255; ++c) {
    CHECK(!isalnum(c) == !h_isalnum(c));
    CHECK(!isalpha(c) == !h_isalpha(c));
    CHECK(!isblank(c) == !h_isblank(c));
    CHECK(!iscntrl(c) == !h_iscntrl(c));
    CHECK(!isdigit(c) == !h_isdigit(c));
    CHECK(!isgraph(c) == !h_isgraph(c));
    CHECK(!islower(c) == !h_islower(c));
    CHECK(!isprint(c) == !h_isprint(c));
    CHECK(!ispunct(c) == !h_ispunct(c));
    CHECK(!isspace(c) == !h_isspace(c));
    CHECK(!isupper(c) == !h_isupper(c));
    CHECK(!isxdigit(c) == !h_isxdigit(c));
    CHECK(tolower(c) == h_tolower(c));
    CHECK(toupper(c) == h_toupper(c));
    if (!case_ok) {
      printf("  first mismatch at c=%d\n", c);
      break;
    }
  }
  /* Out-of-domain inputs: defined by our header, must not crash */
  CHECK(isalpha(256) == 0 && isalpha(-129) == 0);
  CHECK(tolower(1000) == 1000 && toupper(-1000) == -1000);
  CHECK(tolower(-1) == -1 && toupper(-1) == -1);
  /* Audit regression A-2: the header promises EVERY negative
   * classifies as the byte with the same low eight bits; the
   * old floor at -128 silently returned "nothing" below it.
   * -191 is 'A', -200 is '8', -129 is DEL, -256 is NUL, and
   * EOF (-1, low byte 0xff) still classifies as nothing. The
   * calls go through volatile function pointers: this compiler
   * otherwise answers classifier calls itself (isdigit becomes
   * an inline range test, literals get folded with the host's
   * semantics) and the library under test never runs. The
   * symbol-prefixed ASan build below makes the same checks
   * against real calls by construction. */
  {
    int (*volatile fp_alpha)(int) = isalpha;
    int (*volatile fp_upper)(int) = isupper;
    int (*volatile fp_digit)(int) = isdigit;
    int (*volatile fp_cntrl)(int) = iscntrl;
    volatile int va = -191, vd = -200, vdel = -129, vnul = -256;
    volatile int veof = -1;
    CHECK(fp_alpha(va) != 0 && fp_upper(va) != 0);
    CHECK(fp_digit(vd) != 0);
    CHECK(fp_cntrl(vdel) != 0);
    CHECK(fp_cntrl(vnul) != 0);
    CHECK(fp_alpha(veof) == 0 && fp_cntrl(veof) == 0);
  }
  CASE_END();
}

static void case_strerror(void) {
  CASE_BEGIN("strerror sanity over the defined codes");
  static const int codes[] = {
    EPERM, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF,
    ECHILD, EAGAIN, ENOMEM, EACCES, EFAULT, EBUSY, EEXIST, EXDEV,
    ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE, ENOTTY, ETXTBSY,
    EFBIG, ENOSPC, ESPIPE, EROFS, EMLINK, EPIPE, EDOM, ERANGE,
    EDEADLK, ENAMETOOLONG, ENOLCK, ENOSYS, ENOTEMPTY, ELOOP, ENOMSG,
    EIDRM, EILSEQ, ENOTSUP, EOVERFLOW, ECANCELED, EOWNERDEAD,
    ENOTRECOVERABLE, EADDRINUSE, EADDRNOTAVAIL, EAFNOSUPPORT,
    EALREADY, EBADMSG, ECONNABORTED, ECONNREFUSED, ECONNRESET,
    EDESTADDRREQ, EDQUOT, EHOSTUNREACH, EINPROGRESS, EISCONN,
    EMSGSIZE, EMULTIHOP, ENETDOWN, ENETRESET, ENETUNREACH, ENOBUFS,
    ENOLINK, ENOPROTOOPT, ENOTCONN, ENOTSOCK, EPROTO,
    EPROTONOSUPPORT, EPROTOTYPE, ETIMEDOUT, 0,
  };
  const size_t ncodes = sizeof codes / sizeof codes[0];
  size_t i, j;
  for (i = 0; i < ncodes; ++i) {
    const char* msg = strerror(codes[i]);
    CHECK(msg != NULL && msg[0] != '\0');
    for (j = i + 1; j < ncodes; ++j) {
      /* distinct codes carry distinct messages */
      CHECK(strcmp(msg, strerror(codes[j])) != 0);
    }
  }
  {
    const char* unknown = strerror(9999);
    CHECK(strstr(unknown, "Unknown error") == unknown);
    CHECK(strstr(unknown, "9999") != NULL);
    CHECK(strstr(strerror(-3), "-3") != NULL);
  }
  CASE_END();
}

int main(void) {
  /* The default heap goes in before anything can print: this
   * binary links LithoLibC's allocation family under the same
   * names the host library uses, so any allocation the host makes
   * through the public names lands on WaferHeap. */
  setup_heap();
  if (!load_host_refs()) {
    printf("FAIL: could not load host libc references\n");
    return 1;
  }
  case_malloc_family();
  case_strdup();
  case_memory();
  case_memmove();
  case_string();
  case_strtok();
  case_ctype();
  case_strerror();
  printf("%s (%d failure%s)\n",
         g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
