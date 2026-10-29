/* bigint.c ... see litholibc_bigint.h. Straightforward schoolbook
 * arithmetic; these routines run at logging and parsing speeds,
 * not in a render loop, and obvious correctness outranks speed.
 */

#include "litholibc_bigint.h"

#include <string.h>

void litho_big_zero(litho_big_t* a) {
  a->n = 0;
  memset(a->w, 0, sizeof a->w);
}

void litho_big_from_u64(litho_big_t* a, uint64_t v) {
  litho_big_zero(a);
  if (v == 0) return;
  a->w[0] = (uint32_t)v;
  a->w[1] = (uint32_t)(v >> 32);
  a->n = a->w[1] ? 2 : 1;
}

int litho_big_is_zero(const litho_big_t* a) { return a->n == 0; }

static void big_trim(litho_big_t* a) {
  while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

int litho_big_bitlen(const litho_big_t* a) {
  uint32_t top;
  int bits;
  if (a->n == 0) return 0;
  top = a->w[a->n - 1];
  bits = (a->n - 1) * 32;
  while (top) {
    bits++;
    top >>= 1;
  }
  return bits;
}

int litho_big_test_bit(const litho_big_t* a, int i) {
  int word = i / 32;
  if (i < 0 || word >= a->n) return 0;
  return (int)((a->w[word] >> (i % 32)) & 1u);
}

int litho_big_cmp(const litho_big_t* a, const litho_big_t* b) {
  int i;
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (i = a->n - 1; i >= 0; i--) {
    if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
  }
  return 0;
}

void litho_big_sub(litho_big_t* a, const litho_big_t* b) {
  uint64_t borrow = 0;
  int i;
  for (i = 0; i < a->n; i++) {
    uint64_t bi = i < b->n ? b->w[i] : 0;
    uint64_t cur = (uint64_t)a->w[i] - bi - borrow;
    a->w[i] = (uint32_t)cur;
    borrow = (cur >> 63) & 1u; /* wrapped negative means we borrowed */
  }
  big_trim(a);
}

int litho_big_mul_small(litho_big_t* a, uint32_t m) {
  uint64_t carry = 0;
  int i;
  if (a->n == 0 || m == 1) return 0;
  if (m == 0) {
    litho_big_zero(a);
    return 0;
  }
  for (i = 0; i < a->n; i++) {
    uint64_t cur = (uint64_t)a->w[i] * m + carry;
    a->w[i] = (uint32_t)cur;
    carry = cur >> 32;
  }
  if (carry) {
    if (a->n >= LITHO_BIG_WORDS) return -1;
    a->w[a->n++] = (uint32_t)carry;
  }
  return 0;
}

int litho_big_mul_pow5(litho_big_t* a, int e) {
  /* 5^13 = 1220703125 < 2^31: the largest chunk mul_small takes
   * comfortably. */
  static const uint32_t pow5[13] = {1u,        5u,        25u,      125u,
                                    625u,      3125u,     15625u,   78125u,
                                    390625u,   1953125u,  9765625u, 48828125u,
                                    244140625u};
  while (e >= 13) {
    if (litho_big_mul_small(a, 1220703125u) != 0) return -1;
    e -= 13;
  }
  if (e > 0) return litho_big_mul_small(a, pow5[e]);
  return 0;
}

int litho_big_mul_pow10(litho_big_t* a, int e) {
  /* 10^9 < 2^30. */
  static const uint32_t pow10[9] = {1u,       10u,       100u,      1000u,
                                    10000u,   100000u,   1000000u,  10000000u,
                                    100000000u};
  while (e >= 9) {
    if (litho_big_mul_small(a, 1000000000u) != 0) return -1;
    e -= 9;
  }
  if (e > 0) return litho_big_mul_small(a, pow10[e]);
  return 0;
}

void litho_big_shl(litho_big_t* a, int bits) {
  int words, rem, i;
  if (a->n == 0 || bits == 0) return;
  words = bits / 32;
  rem = bits % 32;
  for (i = LITHO_BIG_WORDS - 1; i >= 0; i--) {
    uint32_t v = 0;
    int src = i - words;
    /* The two terms take independent guards: at the top target
     * (src == a->n) the main term is an implicit zero, but the
     * carry out of the old top word is real and must land. An
     * earlier version guarded both terms with src < a->n and
     * silently dropped that carry word on every shift, which
     * truncated dtoa expansions, strtod alignments and the
     * fmod grid alike. */
    if (src >= 0 && src < a->n) v = a->w[src] << rem;
    if (rem && src - 1 >= 0 && src - 1 < a->n) {
      v |= a->w[src - 1] >> (32 - rem);
    }
    a->w[i] = v;
  }
  a->n = LITHO_BIG_WORDS;
  big_trim(a);
}

void litho_big_shr(litho_big_t* a, int bits) {
  int words, rem, i, newn;
  if (a->n == 0 || bits == 0) return;
  words = bits / 32;
  rem = bits % 32;
  if (words >= a->n) {
    litho_big_zero(a);
    return;
  }
  newn = a->n - words;
  for (i = 0; i < newn; i++) {
    uint32_t v = a->w[i + words] >> rem;
    if (rem && i + words + 1 < a->n) v |= a->w[i + words + 1] << (32 - rem);
    a->w[i] = v;
  }
  a->n = newn;
  big_trim(a);
}

uint32_t litho_big_divmod_small(litho_big_t* a, uint32_t m) {
  uint64_t rem = 0;
  int i;
  for (i = a->n - 1; i >= 0; i--) {
    uint64_t cur = (rem << 32) | a->w[i];
    a->w[i] = (uint32_t)(cur / m);
    rem = cur % m;
  }
  big_trim(a);
  return (uint32_t)rem;
}

void litho_big_divmod(const litho_big_t* num, const litho_big_t* den,
                      litho_big_t* q, litho_big_t* r) {
  litho_big_t rem, dshift;
  int shift, i;
  litho_big_zero(q);
  *r = *num;
  if (litho_big_cmp(num, den) < 0) return;
  shift = litho_big_bitlen(num) - litho_big_bitlen(den);
  dshift = *den;
  litho_big_shl(&dshift, shift);
  rem = *num;
  for (i = shift; i >= 0; i--) {
    if (litho_big_cmp(&rem, &dshift) >= 0) {
      litho_big_sub(&rem, &dshift);
      if (i / 32 < LITHO_BIG_WORDS) q->w[i / 32] |= (uint32_t)1u << (i % 32);
      if (i / 32 + 1 > q->n) q->n = i / 32 + 1;
    }
    litho_big_shr(&dshift, 1);
  }
  big_trim(q);
  *r = rem;
}

int litho_big_to_digits(const litho_big_t* a, char* out) {
  /* Work in base 10^9 chunks, low chunk first, then print the top
   * chunk plainly and the rest zero padded to 9. */
  litho_big_t t;
  uint32_t chunks[LITHO_BIG_MAX_DIGITS / 9 + 2];
  int nch = 0, pos = 0, i, j;
  if (a->n == 0) {
    out[0] = '0';
    out[1] = '\0';
    return 1;
  }
  t = *a;
  while (t.n > 0) {
    chunks[nch++] = litho_big_divmod_small(&t, 1000000000u);
  }
  /* Top chunk: no padding. */
  {
    uint32_t c = chunks[nch - 1];
    char tmp[10];
    int tn = 0;
    while (c) {
      tmp[tn++] = (char)('0' + (int)(c % 10));
      c /= 10;
    }
    while (tn) out[pos++] = tmp[--tn];
  }
  for (i = nch - 2; i >= 0; i--) {
    uint32_t c = chunks[i];
    for (j = 8; j >= 0; j--) {
      out[pos + j] = (char)('0' + (int)(c % 10));
      c /= 10;
    }
    pos += 9;
  }
  out[pos] = '\0';
  return pos;
}
