/* string.c ... LithoLibC string functions.
 *
 * Plain, obviously correct implementations: one pass, no state, no
 * tricks beyond a small stack bitmap for the span family. strdup
 * and strndup live in malloc.c with the rest of the allocation
 * family; strerror has its own file beside errno.c.
 */

#include <string.h>

size_t strlen(const char* s) {
  const char* p = s;
  while (*p) ++p;
  return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t maxlen) {
  size_t i;
  for (i = 0; i < maxlen; ++i) {
    if (s[i] == '\0') break;
  }
  return i;
}

char* strcpy(char* dest, const char* src) {
  char* out = dest;
  while ((*dest++ = *src++) != '\0') {
  }
  return out;
}

char* strncpy(char* dest, const char* src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; ++i) dest[i] = src[i];
  /* The standard asks for the remainder to be zero filled, always. */
  for (; i < n; ++i) dest[i] = '\0';
  return dest;
}

char* strcat(char* dest, const char* src) {
  strcpy(dest + strlen(dest), src);
  return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
  char* end = dest + strlen(dest);
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; ++i) end[i] = src[i];
  end[i] = '\0';
  return dest;
}

int strcmp(const char* a, const char* b) {
  /* Compare as unsigned char, as the standard requires, so bytes
   * above 0x7f order by their byte value on every platform. */
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  while (*x != '\0' && *x == *y) {
    ++x;
    ++y;
  }
  return (int)*x - (int)*y;
}

int strncmp(const char* a, const char* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  size_t i;
  for (i = 0; i < n; ++i) {
    if (x[i] != y[i] || x[i] == '\0') return (int)x[i] - (int)y[i];
  }
  return 0;
}

char* strchr(const char* s, int c) {
  const char want = (char)c;
  for (;; ++s) {
    if (*s == want) return (char*)s;
    if (*s == '\0') return NULL;
  }
}

char* strrchr(const char* s, int c) {
  const char want = (char)c;
  const char* last = NULL;
  for (;; ++s) {
    if (*s == want) last = s;
    if (*s == '\0') return (char*)last;
  }
}

/* Two loops, no preprocessing table: needle lengths in this
 * library's world are short, and the naive search is the one whose
 * correctness needs no argument. */
char* strstr(const char* haystack, const char* needle) {
  const size_t nlen = strlen(needle);
  const char* p;
  if (nlen == 0) return (char*)haystack;
  for (p = haystack; (p = strchr(p, needle[0])) != NULL; ++p) {
    if (strncmp(p, needle, nlen) == 0) return (char*)p;
  }
  return NULL;
}

/* Membership bitmap over the byte range, built per call. */
static void lk_build_set(unsigned char set[32], const char* chars) {
  size_t i;
  memset(set, 0, 32);
  for (i = 0; chars[i] != '\0'; ++i) {
    const unsigned char b = (unsigned char)chars[i];
    set[b >> 3] |= (unsigned char)(1u << (b & 7));
  }
}

static int lk_in_set(const unsigned char set[32], unsigned char b) {
  return (set[b >> 3] >> (b & 7)) & 1;
}

size_t strspn(const char* s, const char* accept) {
  unsigned char set[32];
  size_t n = 0;
  lk_build_set(set, accept);
  while (s[n] != '\0' && lk_in_set(set, (unsigned char)s[n])) ++n;
  return n;
}

size_t strcspn(const char* s, const char* reject) {
  unsigned char set[32];
  size_t n = 0;
  lk_build_set(set, reject);
  while (s[n] != '\0' && !lk_in_set(set, (unsigned char)s[n])) ++n;
  return n;
}

char* strpbrk(const char* s, const char* accept) {
  unsigned char set[32];
  lk_build_set(set, accept);
  for (; *s != '\0'; ++s) {
    if (lk_in_set(set, (unsigned char)*s)) return (char*)s;
  }
  return NULL;
}

char* strtok_r(char* s, const char* delim,
               char** saveptr) {
  char* token;
  if (s == NULL) s = *saveptr;
  if (s == NULL) return NULL;
  s += strspn(s, delim); /* skip leading delimiters */
  if (*s == '\0') {
    *saveptr = s;
    return NULL;
  }
  token = s;
  s += strcspn(s, delim); /* token runs to the next delimiter */
  if (*s != '\0') {
    *s = '\0';
    *saveptr = s + 1;
  } else {
    *saveptr = s;
  }
  return token;
}

/* strtok keeps its scan position in one static, the classic shape.
 * It is not thread-safe and cannot be made so without changing its
 * interface; LithoThread's arrival does not change that, and the
 * documentation says so. Callers that need reentrancy use strtok_r. */
char* strtok(char* s, const char* delim) {
  static char* lk_strtok_save;
  return strtok_r(s, delim, &lk_strtok_save);
}

/* C locale collation is codepoint order: strcoll IS strcmp here.
 * If a real locale ever arrives, these two functions are the seam
 * where its tables plug in, and nowhere else needs to know. */
int strcoll(const char* a, const char* b) { return strcmp(a, b); }

size_t strxfrm(char* dest, const char* src, size_t n) {
  const size_t len = strlen(src);
  if (dest != NULL && n > 0) {
    /* Copy what fits, always terminating when there is room. */
    const size_t take = len < n - 1 ? len : n - 1;
    memcpy(dest, src, take);
    dest[take] = '\0';
  }
  return len;
}
