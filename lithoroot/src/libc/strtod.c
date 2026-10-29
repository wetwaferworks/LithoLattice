/* strtod.c ... strtod, strtof, atof: exact decimal (and hex) to
 * binary conversion.
 *
 * Method: the subject sequence is parsed into an integer D of at
 * most 800 significant decimal digits (further digits fold into
 * the decimal exponent and a sticky flag) and a decimal exponent
 * E with value = (D + f) x 10^E, 0 <= f < 1, f > 0 iff sticky.
 * Conversion then happens in exact bigint arithmetic:
 *
 *   E >= 0: P = D x 10^E exactly, and P is rounded from its bits.
 *   E <  0: Q and R come from a full bigint division of D (or a
 *           shifted D) by 10^-E, and rounding compares 2R to the
 *           divisor.
 *
 * Rounding is round-to-nearest-even, with one refinement: at an
 * exact tie, a set sticky flag rounds up, because the true value
 * is then strictly above the tie. Keeping 800 digits is what
 * licenses the tie rule in the other cases: by the classic
 * result for binary64 (at most 767 significant decimal digits
 * are ever needed to decide a rounding), a dropped tail smaller
 * than one part in 10^799 can only matter at an exact tie, which
 * is exactly the case the sticky flag resolves. The differential
 * test (tests/libc_stdio_test.c) checks this against the host
 * over sweeps including 900-digit inputs.
 *
 * Hexadecimal floats (0x...) share the finish: the value is an
 * integer times a power of two directly.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "litholibc_bigint.h"

#define DBL_MAX_DIGITS 800

static double make_double(int sign, uint64_t bits) {
  uint64_t u = ((uint64_t)(sign != 0) << 63) | bits;
  double d;
  memcpy(&d, &u, sizeof d);
  return d;
}

/* Extract up to 64 bits of P starting at bit position `shift`
 * (bits beyond the top read as zero). */
static uint64_t big_extract(const litho_big_t* P, long shift) {
  uint64_t out = 0;
  int i;
  for (i = 63; i >= 0; i--) {
    out = (out << 1) | (uint64_t)litho_big_test_bit(P, (int)(shift + i));
  }
  return out;
}

/* Is any bit of P set in [0, count)? */
static int big_any_below(const litho_big_t* P, long count) {
  long i;
  int full_words = (int)(count / 32);
  int rem = (int)(count % 32);
  for (i = 0; i < full_words && i < P->n; i++) {
    if (P->w[i]) return 1;
  }
  if (rem && full_words < P->n) {
    uint32_t mask = (rem == 32) ? 0xffffffffu : ((1u << rem) - 1u);
    if (P->w[full_words] & mask) return 1;
  }
  return 0;
}

/* M = round(P / 2^S) for S >= 1, ties-to-even, an exact tie
 * rounding up when sticky is set. The caller guarantees the
 * result fits in 64 bits with room for the round-up. */
static uint64_t big_round_shift(const litho_big_t* P, long S, int sticky) {
  uint64_t kept = big_extract(P, S);
  int half = litho_big_test_bit(P, (int)(S - 1));
  int lower = big_any_below(P, S - 1);
  if (half && (lower || sticky || (kept & 1u))) kept++;
  return kept;
}

/* Round the exact value P x 2^e (with sticky as described in the
 * file header) to a double. Sets *range and returns +-HUGE_VAL on
 * overflow, and sets *range for a subnormal or zero result.
 * P must be nonzero. */
static double finish_pow2(litho_big_t* P, long e, int sticky, int sign,
                          int* range) {
  long L = litho_big_bitlen(P);
  long E = e + L - 1; /* value lies in [2^E, 2^(E+1)) */
  *range = 0;
  if (E > 1023) {
    *range = 1;
    return make_double(sign, UINT64_C(0x7ff) << 52);
  }
  if (E >= -1022) {
    long S = L - 53;
    uint64_t kept;
    if (S <= 0) {
      /* P has at most 53 bits: exact (sticky cannot be set here;
       * it needs hundreds of digits, hence a huge P). */
      kept = big_extract(P, 0) << (int)(-S);
    } else {
      kept = big_round_shift(P, S, sticky);
      if (kept >> 53) { /* rounded up out of the binade */
        kept >>= 1;
        E++;
        if (E > 1023) {
          *range = 1;
          return make_double(sign, UINT64_C(0x7ff) << 52);
        }
      }
    }
    return make_double(sign, ((uint64_t)(E + 1023) << 52) |
                                 (kept - (UINT64_C(1) << 52)));
  }
  /* Subnormal or zero: the result grid is 2^-1074. ERANGE is
   * the underflow signal, and the host raises it only when the
   * conversion was inexact down here (an exact subnormal like
   * 0x1p-1074 converts silently): range follows inexactness. */
  {
    long T = e + 1074; /* value = P x 2^T x 2^-1074 */
    uint64_t M;
    int inexact = sticky || (T < 0 && big_any_below(P, -T));
    *range = inexact ? 1 : 0;
    if (T >= 0) {
      M = big_extract(P, 0) << (int)T; /* exact, magnitude-checked
                                          by the branch condition */
    } else {
      M = big_round_shift(P, -T, sticky);
    }
    if (M == 0) return make_double(sign, 0);
    /* M in [1, 2^52]: the bit pattern of M x 2^-1074 is M itself,
     * including M == 2^52, the smallest normal. */
    return make_double(sign, M);
  }
}

/* Round (D / W) x 2^0-ish decimal-negative-exponent values.
 * Computes the double nearest to D / W (with sticky as before).
 * D and W are positive bigints, W >= 10. */
static double finish_div(litho_big_t* D, litho_big_t* W, int sticky, int sign,
                         int* range) {
  /* Find the binade: E with 2^E <= D/W < 2^(E+1). */
  long ld = litho_big_bitlen(D);
  long lw = litho_big_bitlen(W);
  long e = ld - lw; /* true E is e or e-1 */
  litho_big_t shifted;
  int cmp;
  *range = 0;

  /* Compare D against W x 2^e (or D x 2^-e against W). */
  if (e >= 0) {
    shifted = *W;
    litho_big_shl(&shifted, (int)e);
    cmp = litho_big_cmp(D, &shifted);
  } else {
    shifted = *D;
    litho_big_shl(&shifted, (int)(-e));
    cmp = litho_big_cmp(&shifted, W);
  }
  if (cmp < 0) e--;

  if (e > 1023) {
    *range = 1;
    return make_double(sign, UINT64_C(0x7ff) << 52);
  }
  if (e >= -1022) {
    /* M = round(D x 2^(52-e) / W), landing in [2^52, 2^53]. */
    long s = 52 - e;
    litho_big_t num, den, qbig, rbig;
    uint64_t Q;
    if (s >= 0) {
      num = *D;
      litho_big_shl(&num, (int)s);
      den = *W;
    } else {
      num = *D;
      den = *W;
      litho_big_shl(&den, (int)(-s));
    }
    litho_big_divmod(&num, &den, &qbig, &rbig);
    Q = big_extract(&qbig, 0);
    /* Round: compare 2R against den. */
    litho_big_shl(&rbig, 1);
    cmp = litho_big_cmp(&rbig, &den);
    if (cmp > 0 || (cmp == 0 && (sticky || (Q & 1u)))) Q++;
    if (Q >> 53) {
      Q >>= 1;
      e++;
      if (e > 1023) {
        *range = 1;
        return make_double(sign, UINT64_C(0x7ff) << 52);
      }
    }
    return make_double(sign, ((uint64_t)(e + 1023) << 52) |
                                 (Q - (UINT64_C(1) << 52)));
  }
  /* Subnormal: M = round(D x 2^1074 / W) on the 2^-1074 grid.
   * ERANGE only when inexact (nonzero remainder or sticky), as
   * in finish_pow2. */
  {
    litho_big_t num, qbig, rbig;
    uint64_t M;
    int c2;
    num = *D;
    litho_big_shl(&num, 1074);
    litho_big_divmod(&num, W, &qbig, &rbig);
    *range = (sticky || !litho_big_is_zero(&rbig)) ? 1 : 0;
    M = big_extract(&qbig, 0);
    litho_big_shl(&rbig, 1);
    c2 = litho_big_cmp(&rbig, W);
    if (c2 > 0 || (c2 == 0 && (sticky || (M & 1u)))) M++;
    if (M == 0) return make_double(sign, 0);
    return make_double(sign, M);
  }
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int ci_equal(const char* p, const char* word, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (tolower((unsigned char)p[i]) != word[i]) return 0;
  }
  return 1;
}

double strtod(const char* s, char** endptr) {
  const char* p = s;
  const char* subject;
  int sign = 0, range = 0;
  double result;

  while (isspace((unsigned char)*p)) p++;
  subject = p;
  if (*p == '+' || *p == '-') {
    sign = (*p == '-');
    p++;
  }

  /* inf / infinity / nan */
  if (ci_equal(p, "inf", 3)) {
    const char* end = p + 3;
    if (ci_equal(p, "infinity", 8)) end = p + 8;
    if (endptr) *endptr = (char*)end;
    return make_double(sign, UINT64_C(0x7ff) << 52);
  }
  if (ci_equal(p, "nan", 3)) {
    const char* end = p + 3;
    if (*end == '(') {
      const char* q = end + 1;
      while (isalnum((unsigned char)*q) || *q == '_') q++;
      if (*q == ')') end = q + 1;
    }
    if (endptr) *endptr = (char*)end;
    /* A quiet NaN with the standard payload-free bit pattern. */
    return make_double(sign, (UINT64_C(0x7ff) << 52) | (UINT64_C(1) << 51));
  }

  /* Hexadecimal float: 0x... */
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    const char* q = p + 2;
    const char* digits_start = q;
    litho_big_t H;
    long frac_hex = 0, kept_bits = 0, dropped_bits = 0;
    int sticky = 0, any = 0, seen_point = 0;
    long pexp = 0;
    litho_big_zero(&H);
    while (1) {
      int hv = hexval((unsigned char)*q);
      if (hv >= 0) {
        any = 1;
        if (seen_point) frac_hex++;
        if (hv == 0 && litho_big_is_zero(&H)) {
          /* A leading zero: significant for position only (already
           * counted in frac_hex when after the point), free against
           * the bit budget. */
        } else if (kept_bits <= 3968) {
          litho_big_shl(&H, 4);
          H.w[0] |= (uint32_t)hv;
          if (H.n == 0 && hv) H.n = 1;
          kept_bits += 4;
        } else {
          dropped_bits += 4;
          if (hv) sticky = 1;
        }
        q++;
        continue;
      }
      if (*q == '.' && !seen_point) {
        seen_point = 1;
        q++;
        continue;
      }
      break;
    }
    if (any) {
      const char* end = q;
      if (*q == 'p' || *q == 'P') {
        const char* r = q + 1;
        int esign = 0;
        long ev = 0;
        int eany = 0;
        if (*r == '+' || *r == '-') {
          esign = (*r == '-');
          r++;
        }
        while (isdigit((unsigned char)*r)) {
          eany = 1;
          if (ev < 100000) ev = ev * 10 + (*r - '0');
          r++;
        }
        if (eany) {
          pexp = esign ? -ev : ev;
          end = r;
        }
      }
      if (endptr) *endptr = (char*)end;
      if (litho_big_is_zero(&H)) return make_double(sign, 0);
      {
        /* value = (H + f) x 2^(pexp - 4 x frac_hex + dropped_bits) */
        long e = pexp - 4 * frac_hex + dropped_bits;
        result = finish_pow2(&H, e, sticky, sign, &range);
        if (range) errno = ERANGE;
        return result;
      }
    }
    /* "0x" with no hex digits: the subject sequence is just "0",
     * handled by the decimal path below with endptr after the 0.
     * digits_start is only used to mark where digits began. */
    (void)digits_start;
  }

  /* Decimal. */
  {
    litho_big_t D;
    long frac_digits = 0;
    long exp_part = 0;
    long sig_kept = 0;    /* significant digits fed into D */
    long dropped = 0;     /* significant digits not fed */
    int sticky = 0;
    int seen_digit = 0, seen_point = 0, leading_done = 0;
    const char* q = p;
    const char* seq_end;

    litho_big_zero(&D);
    while (1) {
      unsigned char c = (unsigned char)*q;
      if (isdigit(c)) {
        int d = c - '0';
        seen_digit = 1;
        if (seen_point) frac_digits++;
        if (d != 0 || leading_done) {
          leading_done = 1;
          if (sig_kept < DBL_MAX_DIGITS) {
            litho_big_mul_small(&D, 10u);
            /* add the digit: D + d */
            {
              uint64_t carry = (uint64_t)d;
              int i = 0;
              while (carry) {
                if (i >= D.n) {
                  D.w[D.n++] = 0;
                }
                carry += D.w[i];
                D.w[i] = (uint32_t)carry;
                carry >>= 32;
                i++;
              }
              if (D.n == 0) D.n = 1;
            }
            sig_kept++;
          } else {
            dropped++;
            if (d) sticky = 1;
          }
        }
        q++;
        continue;
      }
      if (c == '.' && !seen_point) {
        seen_point = 1;
        q++;
        continue;
      }
      break;
    }
    if (!seen_digit) {
      if (endptr) *endptr = (char*)s;
      return 0.0;
    }
    seq_end = q;
    if (*q == 'e' || *q == 'E') {
      const char* r = q + 1;
      int esign = 0, eany = 0;
      long ev = 0;
      if (*r == '+' || *r == '-') {
        esign = (*r == '-');
        r++;
      }
      while (isdigit((unsigned char)*r)) {
        eany = 1;
        if (ev < 100000) ev = ev * 10 + (*r - '0');
        r++;
      }
      if (eany) {
        exp_part = esign ? -ev : ev;
        seq_end = r;
      }
    }
    if (endptr) *endptr = (char*)seq_end;
    (void)subject;
    if (litho_big_is_zero(&D)) return make_double(sign, 0);

    /* value = (D + f) x 10^E */
    {
      long E = exp_part - frac_digits + dropped;
      /* Decimal magnitude: D has sig_kept digits, so the value
       * lies in [10^(mag-1), 10^mag) with mag = sig_kept + E (the
       * dropped digits are already accounted inside E). */
      long mag = sig_kept + E; /* value < 10^mag */
      if (mag - 1 >= 309) {
        errno = ERANGE;
        return make_double(sign, (UINT64_C(0x7ff) << 52)); /* HUGE_VAL */
      }
      if (mag <= -324) {
        errno = ERANGE;
        return make_double(sign, 0);
      }
      if (E >= 0) {
        litho_big_t P = D;
        (void)litho_big_mul_pow10(&P, (int)E);
        result = finish_pow2(&P, 0, sticky, sign, &range);
        if (range) errno = ERANGE;
        return result;
      } else {
        litho_big_t W;
        litho_big_from_u64(&W, 1);
        (void)litho_big_mul_pow10(&W, (int)(-E));
        result = finish_div(&D, &W, sticky, sign, &range);
        if (range) errno = ERANGE;
        return result;
      }
    }
  }
}

float strtof(const char* s, char** endptr) {
  double d = strtod(s, endptr);
  float f = (float)d;
  if (d != 0.0) {
    double av = d < 0 ? -d : d;
    float af = f < 0 ? -f : f;
    /* Overflow to float infinity, or a nonzero double landing in
     * float subnormal / zero territory: ERANGE, as the host does. */
    if (af > 3.4028234663852886e38f || (av < 1.1754943508222875e-38 &&
                                        av != (double)af)) {
      errno = ERANGE;
    }
  }
  return f;
}

double atof(const char* s) { return strtod(s, NULL); }
