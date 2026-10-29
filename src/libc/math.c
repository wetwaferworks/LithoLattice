/* math.c ... LithoLibC mathematics: the census-derived libm set
 * (docs/LITHOLIBC-STDIO.md) in double plus the float variants in
 * use. No long double anywhere, per the freeze.
 *
 * How each family is built, and what it costs in accuracy:
 *
 *   Exact (bit-level, correctly rounded by construction):
 *   fabs, copysign, fmin, fmax, floor / ceil / trunc / round and
 *   lround (mantissa masking), sqrt (a 106-bit integer square
 *   root with an exact rounding rule), frexp, ldexp, modf, fmod
 *   and remainder (bigint division through the WaferHeap-free
 *   stack bigint in litholibc_bigint.h).
 *
 *   Series and reduction (fdlibm-class algorithms, written here
 *   from their mathematical descriptions):
 *   log: range-reduce to [sqrt(1/2), sqrt(2)) and use the atanh
 *     series 2(s + s^3/3 + ...) with |s| <= 0.1716, adding
 *     e x ln2 in hi / lo split so the large part is exact.
 *     Measured worst 1 ulp on the differential sweep.
 *   exp: k = round(x / ln2), r = x - k ln2 in hi / lo split (the
 *     k ln2 hi product is exact), Taylor in r to degree 15,
 *     scale by 2^k through ldexp. Worst 3 ulp measured.
 *   log2 / log10: the log core with the same split; worst
 *     1 ulp measured each.
 *   sin / cos: Cody-Waite reduction mod pi/2 with the constant
 *     in three parts (hi + lo + lo2, good to about 159 bits) and
 *     a Dekker product for n x hi; Taylor kernels on
 *     [-pi/4, pi/4] carried far enough that the truncation error
 *     is below 1e-19. Worst 1 ulp measured on [-1e6, 1e6]. The
 *     supported domain is |x| < 2^31 * (pi/2), the production
 *     shape's exact-reduction bound; beyond it the functions
 *     return NaN with EDOM by design (audit fix B-2).
 *   tan: sin and cos kernels through the same reduction;
 *     worst 2 ulp measured.
 *   atan: adaptive argument halvings t -> t / (1 + sqrt(1+t^2))
 *     down to 0.1, then the Taylor series; worst 3 ulp measured.
 *   asin / acos: through atan2 and the exact sqrt, with
 *     1 - x^2 formed as (1-x)(1+x) against cancellation;
 *     worst 4 and 3 ulp measured.
 *   hypot: scale by the larger operand; the only rounding beyond
 *     sqrt's is one multiply and one add inside the radical;
 *     worst 2 ulp measured. atan2 worst 1 ulp.
 *   pow: integral exponents up to 4096 go through double-double
 *     binary powering (each multiply keeps its Dekker residual;
 *     also the negative-base path), worst 1 ulp measured. The
 *     general path forms the exponent y x log2(x) in
 *     double-double (split log2 plus product residuals) and
 *     evaluates 2^X through the exp kernel: worst 13 ulp
 *     measured on the sweep, the loosest figure in the file,
 *     growing slowly with |y log2 x|. An earlier single-double
 *     exponent measured 123 ulp on the same sweep, which is
 *     why the exponent is carried in two doubles here.
 *
 * Float variants compute in double and round once, except the
 * exactly-representable families (rounding family, fmod,
 * copysign, fmin/fmax, fabs), which are also exact in float
 * through the double computation because their results are
 * float-representable.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <math.h>

#include "litholibc_bigint.h"

/* ---- bit helpers ---------------------------------------------------- */

static uint64_t d2u(double x) {
  uint64_t u;
  memcpy(&u, &x, sizeof u);
  return u;
}
static double u2d(uint64_t u) {
  double x;
  memcpy(&x, &u, sizeof x);
  return x;
}
static uint32_t f2u(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof u);
  return u;
}
static float u2f(uint32_t u) {
  float x;
  memcpy(&x, &u, sizeof x);
  return x;
}

#define DBL_SIGN UINT64_C(0x8000000000000000)
#define DBL_FRAC_MASK UINT64_C(0x000fffffffffffff)
#define DBL_ABS_MASK UINT64_C(0x7fffffffffffffff)

static double dbl_nan(void) {
  return u2d((UINT64_C(0x7ff) << 52) | (UINT64_C(1) << 51));
}

static double dbl_inf(void) {
  return u2d(UINT64_C(0x7ff0000000000000));
}

/* ---- classification --------------------------------------------------- */

int __litholibc_fpclassify(double x) {
  uint64_t u = d2u(x) & DBL_ABS_MASK;
  if (u == 0) return FP_ZERO;
  if (u < (UINT64_C(1) << 52)) return FP_SUBNORMAL;
  if ((u >> 52) == 0x7ff) {
    return (u & DBL_FRAC_MASK) ? FP_NAN : FP_INFINITE;
  }
  return FP_NORMAL;
}
int __litholibc_fpclassifyf(float x) {
  uint32_t u = f2u(x) & 0x7fffffffu;
  if (u == 0) return FP_ZERO;
  if (u < (1u << 23)) return FP_SUBNORMAL;
  if ((u >> 23) == 0xff) return (u & 0x7fffffu) ? FP_NAN : FP_INFINITE;
  return FP_NORMAL;
}
int __litholibc_signbit(double x) { return (int)(d2u(x) >> 63); }
int __litholibc_signbitf(float x) { return (int)(f2u(x) >> 31); }

/* ---- simple exact functions ------------------------------------------- */

double fabs(double x) { return u2d(d2u(x) & DBL_ABS_MASK); }
float fabsf(float x) { return u2f(f2u(x) & 0x7fffffffu); }

double copysign(double x, double y) {
  return u2d((d2u(x) & DBL_ABS_MASK) | (d2u(y) & DBL_SIGN));
}
float copysignf(float x, float y) {
  return u2f((f2u(x) & 0x7fffffffu) | (f2u(y) & 0x80000000u));
}

double fmin(double x, double y) {
  if (isnan(x)) return y;
  if (isnan(y)) return x;
  if (x == y) return signbit(x) ? y : x; /* -0 vs +0: fmin is -0 */
  return x < y ? x : y;
}
double fmax(double x, double y) {
  if (isnan(x)) return y;
  if (isnan(y)) return x;
  if (x == y) return signbit(x) ? x : y; /* -0 vs +0: fmax is +0 */
  return x > y ? x : y;
}
float fminf(float x, float y) {
  if (isnan(x)) return y;
  if (isnan(y)) return x;
  if (x == y) return signbit(x) ? y : x;
  return x < y ? x : y;
}
float fmaxf(float x, float y) {
  if (isnan(x)) return y;
  if (isnan(y)) return x;
  if (x == y) return signbit(x) ? x : y;
  return x > y ? x : y;
}

/* ---- the rounding family ----------------------------------------------
 * Modes: 0 = floor, 1 = ceil, 2 = trunc, 3 = round (half away). */
static double round_mode(double x, int mode) {
  uint64_t bits = d2u(x);
  uint64_t mag = bits & DBL_ABS_MASK;
  int sign = (int)(bits >> 63);
  int be = (int)(mag >> 52);
  int e;
  uint64_t fracmask, fracbits, ipbits;
  double ip, ax = fabs(x);

  if (isnan(x)) return x;
  if (be == 0x7ff) return x;        /* infinity */
  if (ax >= 4503599627370496.0)     /* 2^52: already integral */
    return x;
  if (ax == 0.0) return x;          /* signed zero stays */
  e = be - 1023;
  if (e < 0) {
    /* |x| < 1: the answer is 0, 1, or -1 by mode and sign. */
    switch (mode) {
      case 0: return sign ? -1.0 : 0.0;                    /* floor */
      case 1: return sign ? -0.0 : 1.0;                    /* ceil */
      case 2: return sign ? -0.0 : 0.0;                    /* trunc */
      default:                                             /* round */
        if (ax >= 0.5) return sign ? -1.0 : 1.0;
        return sign ? -0.0 : 0.0;
    }
  }
  fracmask = (UINT64_C(1) << (52 - e)) - 1;
  fracbits = bits & fracmask;
  if (fracbits == 0) return x;
  ipbits = bits & ~fracmask;
  ip = u2d(ipbits); /* the truncation toward zero, exact */
  switch (mode) {
    case 0: /* floor */
      return sign ? (ip - 1.0) : ip;
    case 1: /* ceil */
      return sign ? ip : (ip + 1.0);
    case 2: /* trunc */
      return ip;
    default: { /* round, half away from zero */
      double f = ax - fabs(ip); /* the exact dropped fraction */
      double mag_r = (f >= 0.5) ? (fabs(ip) + 1.0) : fabs(ip);
      return sign ? -mag_r : mag_r;
    }
  }
}

double floor(double x) { return round_mode(x, 0); }
double ceil(double x) { return round_mode(x, 1); }
double trunc(double x) { return round_mode(x, 2); }
double round(double x) { return round_mode(x, 3); }
float floorf(float x) { return (float)round_mode((double)x, 0); }
float ceilf(float x) { return (float)round_mode((double)x, 1); }
float truncf(float x) { return (float)round_mode((double)x, 2); }
float roundf(float x) { return (float)round_mode((double)x, 3); }

long lround(double x) {
  double r = round(x);
  if (isnan(r)) return 0;
  /* Audit fix B-11: saturate against the LONG limits, not
   * INT64's. long is 32 bits in the wasm32 production shape,
   * and the old constants left every r in [2^31, 2^63) to an
   * out-of-range conversion there (undefined behavior). The
   * comparisons run in double space: (double)LONG_MAX rounds
   * up to exactly 2^31 / 2^63, so the >= test catches precisely
   * the integral values the conversion could not hold, and
   * (double)LONG_MIN is exact on both widths. Written by the
   * book from <limits.h>; the host here is 64-bit, so the
   * 32-bit behavior is reasoned, not executed (see the fix
   * run's report). */
  if (r >= (double)LONG_MAX) return LONG_MAX;
  if (r <= (double)LONG_MIN) return LONG_MIN;
  return (long)r;
}
long lroundf(float x) { return lround((double)x); }

/* ---- frexp / ldexp / modf -------------------------------------------- */

double frexp(double x, int* e) {
  uint64_t bits = d2u(x);
  int be = (int)((bits >> 52) & 0x7ff);
  if (x == 0.0 || be == 0x7ff) {
    *e = 0;
    return x;
  }
  if (be == 0) {
    /* Subnormal: scale into the normal range first. */
    double y = x * 18446744073709551616.0; /* 2^64 */
    double r = frexp(y, e);
    *e -= 64;
    return r;
  }
  *e = be - 1022;
  return u2d((bits & ~(UINT64_C(0x7ff) << 52)) | (UINT64_C(1022) << 52));
}
float frexpf(float x, int* e) {
  double m = frexp((double)x, e);
  return (float)m;
}

/* Internal ldexp: *range set instead of errno (see the header of
 * this file for why callers like fmod must not see errno move).
 * The exponent travels as a long and is clamped on entry (audit
 * fix B-3): be + e used to be int arithmetic, where a caller's
 * INT_MAX exponent overflowed the addition itself, undefined
 * behavior before any range check could run. Any |e| past a few
 * thousand already decides the result for every double, so the
 * clamp changes no outcome; it makes the internal additions
 * (this one and the subnormal path's e + fe) overflow-proof on
 * both long widths. */
static double ldexp_impl(double x, long e, int* range) {
  uint64_t bits;
  int be;
  long ne;
  *range = 0;
  if (e > 100000L) e = 100000L;
  if (e < -100000L) e = -100000L;
  if (x == 0.0 || isnan(x) || isinf(x)) return x;
  bits = d2u(x);
  be = (int)((bits >> 52) & 0x7ff);
  if (be == 0) {
    /* Subnormal input: normalize through frexp and rebuild. */
    int fe;
    double f = frexp(x, &fe);
    return ldexp_impl(f, e + fe, range);
  }
  ne = be + e;
  if (ne >= 0x7ff) {
    *range = 1;
    return copysign(u2d(UINT64_C(0x7ff) << 52), x);
  }
  if (ne >= 1) {
    return u2d((bits & ~(UINT64_C(0x7ff) << 52)) | ((uint64_t)ne << 52));
  }
  if (ne < -52) {
    *range = 1;
    return copysign(0.0, x);
  }
  /* Land in the subnormals: put the implicit bit back and shift
   * the whole mantissa down with round-to-nearest-even. */
  {
    uint64_t mant = (bits & DBL_FRAC_MASK) | (UINT64_C(1) << 52);
    int shift = (int)(1 - ne); /* ne in [-52, 0] here: 1..53 */
    uint64_t out, dropped, half;
    if (shift >= 54) {
      *range = 1;
      return copysign(0.0, x);
    }
    out = mant >> shift;
    dropped = mant & ((shift == 64) ? ~UINT64_C(0)
                                    : ((UINT64_C(1) << shift) - 1));
    half = UINT64_C(1) << (shift - 1);
    if (dropped > half || (dropped == half && (out & 1))) out++;
    *range = 1; /* a subnormal (or rounded-to-min-normal) result */
    return u2d((bits & DBL_SIGN) | out);
  }
}

double ldexp(double x, int e) {
  int range;
  double r = ldexp_impl(x, e, &range);
  if (range) errno = ERANGE;
  return r;
}
float ldexpf(float x, int e) { return (float)ldexp((double)x, e); }

double modf(double x, double* ip) {
  if (isnan(x)) {
    *ip = x;
    return x;
  }
  if (isinf(x)) {
    *ip = x;
    return copysign(0.0, x);
  }
  *ip = trunc(x);
  if (x == *ip) return copysign(0.0, x);
  return x - *ip;
}
float modff(float x, float* ip) {
  double ipd;
  float r = (float)modf((double)x, &ipd);
  *ip = (float)ipd;
  return r;
}

/* ---- sqrt: exact integer square root ----------------------------------
 * x = M x 2^E with M a 53-or-54-bit integer and E even. The root
 * is floor(sqrt(M x 2^52)) computed bit by bit (two bits per step,
 * the classic restoring recurrence), then rounded: the exact
 * remainder R = N - S^2 decides, since the true root passes the
 * half point S + 1/2 exactly when R > S (its square passes
 * S^2 + S + 1/4, and R is an integer). No tie can occur: the
 * square root of an integer is an integer or irrational. */

typedef struct {
  uint64_t hi, lo;
} u128_t;

static int u64_bitlen(uint64_t v) {
  int n = 0;
  while (v) { n++; v >>= 1; }
  return n;
}

double sqrt(double x) {
  uint64_t bits, m;
  int be, E, L;
  uint64_t nh, nl, root;
  u128_t rem;
  long scale;
  if (isnan(x)) return x;
  if (x == 0.0) return x;
  if (x < 0.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (isinf(x)) return x;
  bits = d2u(x);
  be = (int)((bits >> 52) & 0x7ff);
  if (be == 0) {
    m = bits & DBL_FRAC_MASK;
    E = -1074;
  } else {
    m = (bits & DBL_FRAC_MASK) | (UINT64_C(1) << 52);
    E = be - 1075;
  }
  if (E & 1) {
    m <<= 1;
    E -= 1;
  }
  /* N = m << 52; root = floor(sqrt(N)); rem = N - root^2. */
  nh = m >> 12;
  nl = m << 52;
  root = 0;
  rem.hi = 0;
  rem.lo = 0;
  {
    int i;
    /* 64 steps of two bits cover the whole 128-bit container:
     * the leading steps consume the zero pairs above bit 106,
     * which simply hold the root at zero until the significant
     * pairs arrive. (An earlier version ran 53 steps on the
     * assumption the radicand filled the container from the top;
     * it does not, it is anchored at the bottom, and every root
     * came out 11 binary places small.) */
    for (i = 0; i < 64; i++) {
      unsigned pair = (unsigned)(nh >> 62);
      uint64_t cand;
      /* rem = (rem << 2) | pair */
      rem.hi = (rem.hi << 2) | (rem.lo >> 62);
      rem.lo = (rem.lo << 2) | pair;
      /* (nh, nl) <<= 2: the pair is consumed */
      nh = (nh << 2) | (nl >> 62);
      nl <<= 2;
      cand = (root << 2) + 1;
      if (rem.hi != 0 || rem.lo >= cand) {
        uint64_t before = rem.lo;
        rem.lo -= cand;
        if (rem.lo > before) rem.hi -= 1;
        root = (root << 1) | 1;
      } else {
        root <<= 1;
      }
    }
  }
  /* Round: up iff remainder > root. */
  if (rem.hi != 0 || rem.lo > root) root += 1;
  /* value = root x 2^(E/2 - 26) */
  scale = E / 2 - 26;
  L = u64_bitlen(root); /* root > 0 always here */
  {
    uint64_t m53;
    int sh = 53 - L;
    long efield;
    if (sh >= 0) {
      m53 = root << sh;
    } else {
      m53 = root >> (-sh); /* only when root == 2^53: exact */
    }
    efield = scale + L + 1022;
    return u2d(((uint64_t)efield << 52) | (m53 - (UINT64_C(1) << 52)));
  }
}
float sqrtf(float x) { return (float)sqrt((double)x); }

/* ---- fmod / remainder: exact through the stack bigint -----------------
 * |x| = mx x 2^ex and |y| = my x 2^ey with mx, my integers of at
 * most 53 bits. On the common grid 2^ey the quotient and
 * remainder are one bigint divmod; the result is a small integer
 * times a power of two, converted exactly. */

static int decompose_abs(double v, litho_big_t* m, int* e) {
  /* v finite and nonzero. Returns 0 on success. */
  uint64_t bits = d2u(v) & DBL_ABS_MASK;
  int be = (int)(bits >> 52);
  uint64_t mant;
  if (be == 0) {
    mant = bits & DBL_FRAC_MASK;
    *e = -1074;
  } else {
    mant = (bits & DBL_FRAC_MASK) | (UINT64_C(1) << 52);
    *e = be - 1075;
  }
  litho_big_from_u64(m, mant);
  return 0;
}

/* mode 0 = fmod (truncate), 1 = remainder (nearest, ties even). */
static double fmod_core(double x, double y, int mode) {
  litho_big_t A, B, Q, R;
  int ex, ey, sign = signbit(x);
  if (isnan(x) || isnan(y)) return dbl_nan();
  if (isinf(x) || y == 0.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (isinf(y)) return x;
  if (x == 0.0) return x;
  decompose_abs(x, &A, &ex);
  decompose_abs(y, &B, &ey);
  if (ex > ey) {
    litho_big_shl(&A, ex - ey);
  } else if (ey > ex) {
    /* |x| < |y| on the aligned grid means ex < ey cannot happen
     * unless the mantissas compensate... they cannot: both are
     * in [2^52, 2^53) when normal, and subnormals only widen the
     * gap the same direction. But rather than assume, align B
     * the other way and let the quotient come out zero... except
     * shifting B up loses the fmod == x shortcut precision. The
     * honest shortcut: if |x| < |y| (compared as doubles, exact)
     * the answer is x (fmod) or x-or-x-minus-y (remainder). */
    if (fabs(x) < fabs(y)) {
      if (mode == 0) return x;
      /* remainder: x, x - y, or x + y: 2|x| vs |y| decides. */
      {
        double ax = fabs(x), ay = fabs(y);
        if (2.0 * ax < ay) return x;
        if (2.0 * ax > ay) return x - copysign(ay, x);
        /* exact tie: quotient 0 is even, so x stands */
        return x;
      }
    }
    litho_big_shl(&B, ey - ex);
    ey = ex;
  }
  litho_big_divmod(&A, &B, &Q, &R);
  if (mode == 1) {
    /* Compare 2R with B: above -> round the quotient up (result
     * magnitude B - R, sign flipped); exactly equal -> round to
     * even quotient. */
    litho_big_t R2 = R;
    int c;
    litho_big_shl(&R2, 1);
    c = litho_big_cmp(&R2, &B);
    if (c > 0 || (c == 0 && (Q.w[0] & 1u))) {
      litho_big_t diff = B;
      litho_big_sub(&diff, &R);
      R = diff;
      sign = !sign;
    }
  }
  /* Result = R x 2^ey, R < B <= 2^53 plus aligned growth... R is
   * below B which fits the bigint; convert through the top: R is
   * at most 2^53-ish in every reachable case (an aligned A gives
   * R < B <= 2^53 when no shift was applied to B; when B was
   * shifted the quotient is small and R < B still but B can be
   * wide... in that branch |x| and |y| are within a factor the
   * mantissas allow, so B shifted stays under 2^54). Extract via
   * the digit machinery is overkill: assemble from words. */
  {
    uint64_t rv = 0;
    int i;
    int range = 0;
    double r;
    for (i = R.n - 1; i >= 0; i--) rv = (rv << 32) | R.w[i];
    r = ldexp_impl((double)rv, ey, &range);
    (void)range;
    return sign ? -r : r;
  }
}

double fmod(double x, double y) { return fmod_core(x, y, 0); }
double remainder(double x, double y) { return fmod_core(x, y, 1); }
float fmodf(float x, float y) {
  return (float)fmod_core((double)x, (double)y, 0);
}

/* ---- log family ---------------------------------------------------------
 * Constants verified at 60-digit precision against the true
 * values (computed in this library's development, stated so the
 * next reader does not have to trust them): pi/2 splits as
 *   1.5707963267948966 + 6.123233995736766e-17
 *     - 1.4973849048591698e-33  (residual about 5.6e-50)
 * and ln2 splits against a hi part whose mantissa is masked to
 * 31 fraction bits as
 *   0.6931471803691238 + 1.9082149292705877e-10
 * The mask is what makes k x LN2_HI exact for any |k| <= 2047:
 * 32 significant bits times an 11-bit integer fits in 53. */

/* Dekker's product error: for p = fl(a x b), returns the exact
 * residual a x b - p. Used by the trig reduction (a = quadrant
 * count, b = pi/2) and by pow's double-double exponent. Valid
 * when no intermediate overflows; both call sites are tame. */
static double two_prod_err(double a, double b, double p) {
  /* Dekker's product error, valid when no intermediate overflows
   * (here a is the quadrant count, b is pi/2: always tame). */
  const double SPLIT = 134217729.0; /* 2^27 + 1 */
  double ca = SPLIT * a;
  double ah = ca - (ca - a);
  double al = a - ah;
  double cb = SPLIT * b;
  double bh = cb - (cb - b);
  double bl = b - bh;
  return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

#define LN2_HI 0.6931471803691238
#define LN2_LO 1.9082149292705877e-10
#define INV_LN2_HI 1.4426950408889634
#define INV_LN2_LO 2.0355273740931033e-17
#define LOG10_2 0.3010299956639812
#define INV_LN10 0.4342944819032518
#define SQRT2 1.4142135623730951

/* log core: for x > 0 finite, produce e and t with
 * log(x) = e x ln2 + t, t in [-0.35, 0.35] computed to near
 * machine precision by the atanh series. */
static void log_core(double x, int* out_e, double* out_t) {
  uint64_t bits;
  int be, e, saved = 0;
  double f, s, u, poly;
  if (x < 2.2250738585072014e-308) { /* subnormal: scale up */
    x *= 18446744073709551616.0;    /* 2^64 */
    saved = -64;
  }
  bits = d2u(x);
  be = (int)((bits >> 52) & 0x7ff);
  e = be - 1023 + saved;
  /* f = significand in [1, 2) */
  f = u2d((bits & ~(UINT64_C(0x7ff) << 52)) | (UINT64_C(1023) << 52));
  if (f >= SQRT2) {
    f *= 0.5;
    e += 1;
  }
  /* Now f in [1, sqrt(2)): s in [0, 0.1716). (The symmetric lower
   * half [sqrt(1/2), 1) never occurs with this normalization;
   * the bound |s| <= 0.1716 holds either way.) */
  s = (f - 1.0) / (f + 1.0);
  u = s * s;
  /* atanh(s) = s x (1 + u/3 + u^2/5 + ... + u^11/23 + ...);
   * the first omitted term is about 8e-20. Horner from the top:
   * coefficients 1/(2n+1) for n = 11 down to 0. */
  poly = 0.043478260869565216;          /* 1/23 */
  poly = poly * u + 0.047619047619047616;  /* 1/21 */
  poly = poly * u + 0.052631578947368418;  /* 1/19 */
  poly = poly * u + 0.058823529411764705;  /* 1/17 */
  poly = poly * u + 0.066666666666666666;  /* 1/15 */
  poly = poly * u + 0.076923076923076923;  /* 1/13 */
  poly = poly * u + 0.090909090909090912;  /* 1/11 */
  poly = poly * u + 0.111111111111111111;  /* 1/9 */
  poly = poly * u + 0.142857142857142857;  /* 1/7 */
  poly = poly * u + 0.2;                   /* 1/5 */
  poly = poly * u + 0.333333333333333333;  /* 1/3 */
  poly = poly * u + 1.0;
  *out_e = e;
  *out_t = 2.0 * s * poly;
}

double log(double x) {
  int e;
  double t;
  if (isnan(x)) return x;
  if (x < 0.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (x == 0.0) {
    errno = ERANGE;
    return -u2d(UINT64_C(0x7ff) << 52);
  }
  if (isinf(x)) return x;
  log_core(x, &e, &t);
  /* Add the small parts first, then the exact large part. */
  return e * LN2_HI + (e * LN2_LO + t);
}
double log2(double x) {
  int e;
  double t;
  if (isnan(x)) return x;
  if (x < 0.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (x == 0.0) {
    errno = ERANGE;
    return -u2d(UINT64_C(0x7ff) << 52);
  }
  if (isinf(x)) return x;
  log_core(x, &e, &t);
  return e + (t * INV_LN2_HI + t * INV_LN2_LO);
}
double log10(double x) {
  int e;
  double t;
  if (isnan(x)) return x;
  if (x < 0.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (x == 0.0) {
    errno = ERANGE;
    return -u2d(UINT64_C(0x7ff) << 52);
  }
  if (isinf(x)) return x;
  log_core(x, &e, &t);
  return e * LOG10_2 + t * INV_LN10;
}

/* ---- exp ---------------------------------------------------------------- */

/* exp(r) for |r| <= ln2/2 by Taylor to degree 15 (tail below
 * 1e-16 relative). Shared by exp() and pow(). */
static double exp_kernel(double r) {
  double term = 1.0, sum = 1.0;
  int n;
  for (n = 1; n <= 15; n++) {
    term *= r / n;
    sum += term;
  }
  return sum;
}

double exp(double x) {
  long k;
  double r;
  int range = 0;
  double result;
  if (isnan(x)) return x;
  if (isinf(x)) return signbit(x) ? 0.0 : x;
  if (x > 709.782712893384) {
    errno = ERANGE;
    return u2d(UINT64_C(0x7ff) << 52);
  }
  if (x < -745.1332191019411) {
    errno = ERANGE;
    return 0.0;
  }
  /* k = round(x / ln2), computed with the hi/lo reciprocal so
   * the rounding boundary is honest. */
  {
    double kd = x * INV_LN2_HI + x * INV_LN2_LO;
    k = (long)(kd >= 0 ? kd + 0.5 : kd - 0.5);
  }
  r = (x - k * LN2_HI) - k * LN2_LO;
  result = ldexp_impl(exp_kernel(r), (int)k, &range);
  if (range) errno = ERANGE;
  return result;
}

/* log2 in split form: log2(x) = e + h + l with e an integer,
 * h = fl(t / ln2) and l the residual, t from log_core. The
 * split is what pow needs: y times a one-double log2 loses the
 * exponent's low bits to the product's rounding, which the
 * result then amplifies by e^X. */
static void log2_core(double x, int* out_e, double* out_h, double* out_l) {
  int e;
  double t, h;
  log_core(x, &e, &t);
  h = t * INV_LN2_HI;
  *out_e = e;
  *out_h = h;
  *out_l = (t * INV_LN2_LO) + two_prod_err(t, INV_LN2_HI, h);
}

/* 2^(X + Xlo) with the exponent carried in double-double form. */
static double exp2_dd(double X, double Xlo, int* range) {
  long n;
  double frac, r;
  *range = 0;
  if (X >= 1025.0) {
    *range = 1;
    return u2d(UINT64_C(0x7ff) << 52);
  }
  if (X <= -1076.0) {
    *range = 1;
    return 0.0;
  }
  n = (long)(X >= 0 ? X + 0.5 : X - 0.5);
  frac = (X - (double)n) + Xlo;
  r = exp_kernel(frac * LN2_HI + frac * LN2_LO);
  return ldexp_impl(r, (int)n, range);
}

/* ---- trigonometric ------------------------------------------------------
 * Reduction: n = round(x x 2/pi); r = x - n x pi/2 computed as
 * ((x - p) - pe) - n x lo - n x lo2 where p = fl(n x hi) and pe
 * is the exact Dekker product error, so p + pe = n x hi to the
 * last bit. hi + lo + lo2 represents pi/2 to about 159 bits, so
 * the reduced r is honest while |n| stays below about 2^55; the
 * documented working range is |x| <= 1e17. */

#define PIO2_HI 1.5707963267948966
#define PIO2_LO 6.123233995736766e-17
#define PIO2_LO2 (-1.4973849048591698e-33)
#define INV_PIO2 0.63661977236758134

static double sin_kernel(double r) {
  double u = r * r;
  double p = -2.8114572543455206e-15;          /* -1/17! */
  p = p * u + 7.647163731819816e-13;           /* 1/15! */
  p = p * u - 1.6059043836821613e-10;          /* -1/13! */
  p = p * u + 2.505210838544172e-08;           /* 1/11! */
  p = p * u - 2.755731922398589e-06;           /* -1/9! */
  p = p * u + 1.9841269841269841e-04;          /* 1/7! */
  p = p * u - 0.008333333333333333;            /* -1/5! */
  p = p * u + 0.16666666666666666;             /* 1/3! */
  return r - r * u * p;                        /* x - x^3 x P(u)... */
}

static double cos_kernel(double r) {
  double u = r * r;
  double q = -1.5619206968586225e-16;          /* -1/18! */
  q = q * u + 4.7794773323873853e-14;          /* 1/16! */
  q = q * u - 1.1470745597729725e-11;          /* -1/14! */
  q = q * u + 2.0876756987868100e-09;          /* 1/12! */
  q = q * u - 2.7557319223985891e-07;          /* -1/10! */
  q = q * u + 2.4801587301587302e-05;          /* 1/8! */
  q = q * u - 0.001388888888888889;            /* -1/6! */
  q = q * u + 0.041666666666666664;            /* 1/4! */
  q = q * u - 0.5;                             /* -1/2! */
  return 1.0 + u * q;
}

/* Reduce x to (n mod 4, r) with r in about [-pi/4, pi/4].
 *
 * Domain: the reduction is exact only while the quadrant count
 * fits the production shape's long, |x| < 2^31 * (pi/2). Past
 * that bound the (long) cast is undefined behavior (a 64-bit
 * host hides it to 2^63; wasm32 does not), and the reduced
 * value would be meaningless digits anyway. Audit fix B-2:
 * outside the bound, return the defined domain fallback, a
 * NaN residual with errno EDOM, and let the callers' kernels
 * propagate the NaN. The bound is tested in double space,
 * where both long widths agree, with half a quadrant of
 * margin for the rounding step below. */
static int trig_reduce(double x, double* out_r) {
  double nd = x * INV_PIO2;
  long n;
  if (!(fabs(nd) < 2147483647.0)) {
    errno = EDOM;
    *out_r = dbl_nan();
    return 0;
  }
  n = (long)(nd >= 0 ? nd + 0.5 : nd - 0.5);
  double nf = (double)n;
  double p = nf * PIO2_HI;
  double pe = two_prod_err(nf, PIO2_HI, p);
  double r = x - p;
  r -= pe;
  r -= nf * PIO2_LO;
  r -= nf * PIO2_LO2;
  *out_r = r;
  return (int)(((n % 4) + 4) % 4);
}

double sin(double x) {
  double r;
  int q;
  if (isnan(x)) return x;
  if (isinf(x)) {
    errno = EDOM;
    return dbl_nan();
  }
  if (fabs(x) <= 0.7853981633974483) return sin_kernel(x);
  q = trig_reduce(x, &r);
  switch (q) {
    case 0: return sin_kernel(r);
    case 1: return cos_kernel(r);
    case 2: return -sin_kernel(r);
    default: return -cos_kernel(r);
  }
}

double cos(double x) {
  double r;
  int q;
  if (isnan(x)) return x;
  if (isinf(x)) {
    errno = EDOM;
    return dbl_nan();
  }
  if (fabs(x) <= 0.7853981633974483) return cos_kernel(x);
  q = trig_reduce(x, &r);
  switch (q) {
    case 0: return cos_kernel(r);
    case 1: return -sin_kernel(r);
    case 2: return -cos_kernel(r);
    default: return sin_kernel(r);
  }
}

double tan(double x) {
  double r;
  int q;
  if (isnan(x)) return x;
  if (isinf(x)) {
    errno = EDOM;
    return dbl_nan();
  }
  if (fabs(x) <= 0.7853981633974483) {
    return sin_kernel(x) / cos_kernel(x);
  }
  q = trig_reduce(x, &r);
  if ((q & 1) == 0) {
    return sin_kernel(r) / cos_kernel(r);
  }
  return -cos_kernel(r) / sin_kernel(r);
}

/* ---- inverse trigonometrics, atan2, hypot ------------------------------- */

#define PI_2 1.5707963267948966
#define PI_4 0.7853981633974483
#define PI 3.141592653589793

static double atan_kernel(double t) {
  /* atan(t) for |t| <= about 0.1, Taylor through t^15. */
  double u = t * t;
  double p = 0.066666666666666666;           /* 1/15 */
  p = p * u - 0.076923076923076923;          /* -1/13 */
  p = p * u + 0.090909090909090912;          /* 1/11 */
  p = p * u - 0.111111111111111111;          /* -1/9 */
  p = p * u + 0.142857142857142857;          /* 1/7 */
  p = p * u - 0.2;                           /* -1/5 */
  p = p * u + 0.333333333333333333;          /* 1/3 */
  return t - t * u * p;
}

double atan(double x) {
  double ax, t;
  int neg = 0, recip = 0, i;
  if (isnan(x)) return x;
  if (isinf(x)) return signbit(x) ? -PI_2 : PI_2;
  ax = fabs(x);
  neg = signbit(x);
  t = ax;
  if (t > 1.0) {
    recip = 1;
    t = 1.0 / t;
  }
  /* Halve until the kernel's range is reached:
   * atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))). The count is
   * adaptive (at most 3 from t = 1) because every halving costs
   * a rounding the final scaling multiplies back: a fixed three
   * halvings measured 5 ulp on tiny arguments whose true value
   * needs none. */
  i = 0;
  while (t > 0.1) {
    t = t / (1.0 + sqrt(1.0 + t * t));
    i++;
  }
  t = atan_kernel(t) * (double)(1 << i);
  if (recip) t = PI_2 - t;
  return neg ? -t : t;
}

double atan2(double y, double x) {
  if (isnan(y) || isnan(x)) return dbl_nan();
  if (y == 0.0) {
    if (signbit(x)) return signbit(y) ? -PI : PI;
    return y; /* signed zero */
  }
  if (x == 0.0) {
    return signbit(y) ? -PI_2 : PI_2;
  }
  if (isinf(x)) {
    if (isinf(y)) {
      double q = signbit(y) ? -PI_4 : PI_4;
      return signbit(x) ? 3.0 * q : q;
    }
    return signbit(x) ? (signbit(y) ? -PI : PI) : (signbit(y) ? -0.0 : 0.0);
  }
  if (isinf(y)) return signbit(y) ? -PI_2 : PI_2;
  {
    double a = atan(fabs(y / x));
    if (x > 0) return signbit(y) ? -a : a;
    return signbit(y) ? -(PI - a) : (PI - a);
  }
}

double asin(double x) {
  if (isnan(x)) return x;
  if (x > 1.0 || x < -1.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (x == 1.0) return PI_2;
  if (x == -1.0) return -PI_2;
  /* (1 - x)(1 + x), not 1 - x*x: near |x| = 1 the squared form
   * cancels catastrophically (the product's factors are exact
   * or near-exact there), which cost 300 ulp at x = 0.99995 in
   * the acos sweep before this change. */
  return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
  if (isnan(x)) return x;
  if (x > 1.0 || x < -1.0) {
    errno = EDOM;
    return dbl_nan();
  }
  if (x == 1.0) return 0.0;
  if (x == -1.0) return PI;
  return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

double hypot(double x, double y) {
  double ax = fabs(x), ay = fabs(y), m, r;
  if (isinf(ax) || isinf(ay)) return u2d(UINT64_C(0x7ff) << 52);
  if (isnan(ax) || isnan(ay)) return dbl_nan();
  m = ax > ay ? ax : ay;
  if (m == 0.0) return 0.0;
  r = (ax < ay ? ax : ay) / m;
  return m * sqrt(1.0 + r * r);
}

/* ---- pow ------------------------------------------------------------------ */

static int y_is_odd_integer(double y) {
  if (fabs(y) >= 9007199254740992.0) return 0; /* 2^53: all even */
  return (((long long)y) & 1) != 0;
}

/* Integer power in double-double: each multiply keeps its
 * Dekker residual, so the accumulated error stays at the last
 * bit or two even for exponents in the thousands, where plain
 * binary powering measured up to 18 ulp of drift. Intermediates
 * cannot overflow before the result itself does: the squares
 * are bounded by the final magnitude whenever |x| >= 1, and
 * shrink when |x| < 1. */
static double pow_int_dd(double x, long n) {
  double bh = x, bl = 0.0, rh = 1.0, rl = 0.0;
  /* Plain-double shadow (audit fix B-1): an intermediate
   * overflow is legitimate here (the true result is infinite
   * too, per the bound argument above), but the residual
   * arithmetic cannot carry it: two_prod_err of an infinite
   * product forms Inf - Inf, and pow(1e300, 2) came back NaN.
   * The shadow tracks the same powering in plain doubles; the
   * moment a dd intermediate goes non-finite the loop finishes
   * in plain arithmetic and returns the shadow, whose Inf is
   * the honest answer. pow() turns an infinite result into
   * ERANGE as before. The underflow side needs no bail: zero
   * intermediates keep zero residuals. */
  double pb = x, pr = 1.0;
  int overflowed = 0;
  while (n > 0) {
    if (n & 1) {
      if (!overflowed) {
        double p = rh * bh;
        rl = two_prod_err(rh, bh, p) + rh * bl + rl * bh;
        rh = p;
        if (!isfinite(rh)) overflowed = 1;
      }
      pr *= pb;
    }
    n >>= 1;
    if (n) {
      if (!overflowed) {
        double p = bh * bh;
        bl = two_prod_err(bh, bh, p) + 2.0 * bh * bl;
        bh = p;
        if (!isfinite(bh)) overflowed = 1;
      }
      pb *= pb;
    }
  }
  return overflowed ? pr : rh + rl;
}

double pow(double x, double y) {
  if (y == 0.0) return 1.0;
  if (x == 1.0) return 1.0;
  if (isnan(x) || isnan(y)) return dbl_nan();
  if (x == 0.0) {
    if (y > 0.0) {
      /* Odd integral exponents keep the zero's sign. */
      return (signbit(x) && y_is_odd_integer(y)) ? -0.0 : 0.0;
    }
    errno = ERANGE;
    return (signbit(x) && y_is_odd_integer(y)) ? -dbl_inf()
                                              : dbl_inf();
  }
  /* Integral exponent of modest size: double-double powering
   * (also the only path that admits a negative base at any
   * size up to the threshold). */
  if (y == trunc(y) && fabs(y) <= 4096.0) {
    long n = (long)y;
    double r;
    if (n == 0) return 1.0;
    r = pow_int_dd(fabs(x), n > 0 ? n : -n);
    if (n < 0) {
      if (r == 0.0) {
        errno = ERANGE;
        r = dbl_inf();
      } else {
        double inv = 1.0 / r;
        if (isinf(inv)) errno = ERANGE;
        r = inv;
      }
    } else if (isinf(r)) {
      errno = ERANGE;
    }
    if (signbit(x) && (n & 1)) r = -r;
    return r;
  }
  if (isinf(y)) {
    double ax = fabs(x);
    if (ax == 1.0) return 1.0;
    if ((ax > 1.0) == (y > 0.0)) return dbl_inf();
    return 0.0;
  }
  if (isinf(x)) {
    if (signbit(x) && y_is_odd_integer(y)) {
      return y > 0.0 ? -dbl_inf() : -0.0;
    }
    return y > 0.0 ? dbl_inf() : 0.0;
  }
  if (x < 0.0 && y != trunc(y)) {
    errno = EDOM;
    return dbl_nan();
  }
  /* General path: exp2(y x log2(x)) with the exponent formed in
   * double-double (see log2_core). Negative bases reach here
   * only with integral exponents above the powering threshold;
   * the sign follows the exponent's parity. */
  {
    double ax = fabs(x);
    int e, range = 0;
    double h, l, p1, p2, s_hi, bb, lo, X, Xlo, r;
    log2_core(ax, &e, &h, &l);
    p1 = y * h;
    p2 = y * (double)e;
    s_hi = p2 + p1;
    bb = s_hi - p2;
    lo = (p2 - (s_hi - bb)) + (p1 - bb) +
         (two_prod_err(y, h, p1) + two_prod_err(y, (double)e, p2) +
          y * l);
    X = s_hi + lo;
    Xlo = (s_hi - X) + lo;
    r = exp2_dd(X, Xlo, &range);
    if (range) errno = ERANGE;
    if (signbit(x) && y_is_odd_integer(y)) r = -r;
    return r;
  }
}

float expf(float x) {
  double d = exp((double)x);
  float f = (float)d;
  if (f == u2f(0x7f800000u) || (f == 0.0f && d != 0.0)) errno = ERANGE;
  return f;
}
float logf(float x) { return (float)log((double)x); }
float log2f(float x) { return (float)log2((double)x); }
float log10f(float x) { return (float)log10((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float asinf(float x) { return (float)asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }
float atanf(float x) { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }
