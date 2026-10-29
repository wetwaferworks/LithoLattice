/* memory.c ... LithoLibC memory functions: memcpy, memmove,
 * memset, memcmp, memchr.
 *
 * The copy and set functions run word at a time once both sides are
 * word aligned. Words are loaded and stored through the two helpers
 * below, never by casting a byte pointer to a word pointer: on
 * GNU-family compilers the helper is the compiler's own fixed-size
 * copy builtin, which lowers to a plain load or store and can never
 * become a call back into memcpy; elsewhere the word is assembled
 * and disassembled byte by byte, which every compiler still turns
 * into reasonable code and which is defined behavior everywhere.
 * Either way there is no type punning and no alignment assumption
 * beyond what the caller already had.
 */

#include <stdint.h>
#include <string.h>

typedef size_t lk_word;

#if defined(__GNUC__) || defined(__clang__)
static lk_word lk_load_word(const void* p) {
  lk_word w;
  __builtin_memcpy(&w, p, sizeof w);
  return w;
}
static void lk_store_word(void* p, lk_word w) {
  __builtin_memcpy(p, &w, sizeof w);
}
#else
static lk_word lk_load_word(const void* p) {
  const unsigned char* b = (const unsigned char*)p;
  lk_word w = 0;
  size_t i;
  for (i = 0; i < sizeof w; ++i) w |= (lk_word)b[i] << (8 * i);
  return w;
}
static void lk_store_word(void* p, lk_word w) {
  unsigned char* b = (unsigned char*)p;
  size_t i;
  for (i = 0; i < sizeof w; ++i) b[i] = (unsigned char)(w >> (8 * i));
}
#endif

void* memcpy(void* dest, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dest;
  const unsigned char* s = (const unsigned char*)src;
  size_t i;

  /* Head: byte steps until the destination is word aligned (or the
   * copy is done). */
  while (n > 0 && ((uintptr_t)d & (sizeof(lk_word) - 1)) != 0) {
    *d++ = *s++;
    --n;
  }
  /* Bulk: word steps, but only while the source shares the
   * destination's alignment; a permanently skewed pair stays on
   * the byte tail below, which is still correct, just slower. */
  if (((uintptr_t)s & (sizeof(lk_word) - 1)) == 0) {
    while (n >= sizeof(lk_word)) {
      lk_store_word(d, lk_load_word(s));
      d += sizeof(lk_word);
      s += sizeof(lk_word);
      n -= sizeof(lk_word);
    }
  }
  for (i = 0; i < n; ++i) d[i] = s[i];
  return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
  unsigned char* d = (unsigned char*)dest;
  const unsigned char* s = (const unsigned char*)src;
  size_t i;

  if (d == s || n == 0) return dest;
  /* When the ranges sit closer than one word, the word steps below
   * would overlap each other; a plain byte loop in the safe
   * direction is the right tool for that narrow case. */
  {
    const size_t dist = d < s ? (size_t)(s - d) : (size_t)(d - s);
    if (dist < sizeof(lk_word)) {
      if (d < s) {
        for (i = 0; i < n; ++i) d[i] = s[i];
      } else {
        for (i = n; i > 0; --i) d[i - 1] = s[i - 1];
      }
      return dest;
    }
  }
  if (d < s) {
    /* Forward copy cannot clobber unread source. */
    return memcpy(dest, src, n);
  }
  /* Backward copy: start at the far end. Same word scheme, mirrored. */
  while (n > 0 && ((uintptr_t)(d + n) & (sizeof(lk_word) - 1)) != 0) {
    --n;
    d[n] = s[n];
  }
  if (((uintptr_t)(s + n) & (sizeof(lk_word) - 1)) == 0) {
    while (n >= sizeof(lk_word)) {
      n -= sizeof(lk_word);
      lk_store_word(d + n, lk_load_word(s + n));
    }
  }
  for (i = n; i > 0; --i) d[i - 1] = s[i - 1];
  return dest;
}

void* memset(void* dest, int c, size_t n) {
  unsigned char* d = (unsigned char*)dest;
  const unsigned char byte = (unsigned char)c;
  lk_word pattern = byte;
  size_t i;

  /* Replicate the byte across a word by doubling shifts. */
  for (i = 1; i < sizeof(lk_word); i <<= 1) pattern |= pattern << (8 * i);

  while (n > 0 && ((uintptr_t)d & (sizeof(lk_word) - 1)) != 0) {
    *d++ = byte;
    --n;
  }
  while (n >= sizeof(lk_word)) {
    lk_store_word(d, pattern);
    d += sizeof(lk_word);
    n -= sizeof(lk_word);
  }
  for (i = 0; i < n; ++i) d[i] = byte;
  return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char* x = (const unsigned char*)a;
  const unsigned char* y = (const unsigned char*)b;
  size_t i;
  for (i = 0; i < n; ++i) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}

void* memchr(const void* s, int c, size_t n) {
  const unsigned char* p = (const unsigned char*)s;
  const unsigned char want = (unsigned char)c;
  size_t i;
  for (i = 0; i < n; ++i) {
    if (p[i] == want) return (void*)(p + i);
  }
  return NULL;
}
