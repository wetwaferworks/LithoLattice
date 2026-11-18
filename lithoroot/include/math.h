/* math.h ... the LithoLibC math header.
 *
 * Declares exactly what src/libc/math.c implements: the set the
 * engine and game actually call (derived by census; see
 * docs/LITHOLIBC-STDIO.md), in double plus the float variants in
 * use. No long double variants anywhere in the surface: the
 * LithoSys freeze leaves long double out.
 *
 * Accuracy (measured, tests/libc_stdio_test.c): the elementary
 * set (sqrt, floor family, fmod, remainder, copysign, ldexp,
 * frexp, modf) is exact. The transcendentals are portable
 * series / argument-reduction implementations written for this
 * library. Measured worst cases on the differential sweeps:
 * log, log2, log10, sin, cos and atan2 within 1 ulp; tan and
 * hypot within 2; exp, acos, atan and the near-1 log within 3;
 * asin within 4; pow within 1 ulp on integral exponents
 * (double-double powering) and 13 ulp on the general
 * double-double exp2 path. The doc carries the per-function
 * table with the sweep shapes.
 *
 * Domain note (audit fix B-2, 2026-10-28): sin, cos and tan
 * reduce exactly for |x| < 2^31 * (pi/2), the production
 * shape's bound. Beyond it they return NaN with errno EDOM by
 * design; the out-of-range cast and the meaningless reduction
 * it fed are gone.
 */
#ifndef LITHOLIBC_MATH_H
#define LITHOLIBC_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#else
/* Fallback for other compilers (notably MSVC on the native side):
 * every compiler we target folds these to the IEEE values. */
#define HUGE_VAL  (1.0 / 0.0)
#define HUGE_VALF (1.0f / 0.0f)
#define INFINITY  (1.0f / 0.0f)
#define NAN       (0.0f / 0.0f)
#endif

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

/* Classification. fpclassify and friends are macros over the bit
 * pattern, per C99. The values are the conventional ones. */
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

#define fpclassify(x)                                                     \
  (sizeof(x) == sizeof(float)                                             \
       ? __litholibc_fpclassifyf((float)(x))                              \
       : __litholibc_fpclassify((double)(x)))
#define isnan(x) (fpclassify(x) == FP_NAN)
#define isinf(x) (fpclassify(x) == FP_INFINITE)
#define isfinite(x) (fpclassify(x) > FP_INFINITE)
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x)                                                        \
  (sizeof(x) == sizeof(float) ? __litholibc_signbitf((float)(x))          \
                              : __litholibc_signbit((double)(x)))

int __litholibc_fpclassify(double x);
int __litholibc_fpclassifyf(float x);
int __litholibc_signbit(double x);
int __litholibc_signbitf(float x);

/* ---- exactly implemented ----------------------------------------- */
double sqrt(double x);
float sqrtf(float x);
double fabs(double x);
float fabsf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double trunc(double x);
float truncf(float x);
double round(double x);
float roundf(float x);
long lround(double x);
long lroundf(float x);
/* 2026-11-16 (S5): llround/llroundf over round (halfway cases away from zero, as C requires;
 * out-of-range results are unspecified, as in C). */
static inline long long llround(double x) { return (long long)round(x); }
static inline long long llroundf(float x) { return (long long)round((double)x); }
double fmod(double x, double y);
float fmodf(float x, float y);
double remainder(double x, double y);
double copysign(double x, double y);
float copysignf(float x, float y);
double fmin(double x, double y);
float fminf(float x, float y);
double fmax(double x, double y);
float fmaxf(float x, float y);
double ldexp(double x, int e);
float ldexpf(float x, int e);
double frexp(double x, int* e);
float frexpf(float x, int* e);
double modf(double x, double* ip);
float modff(float x, float* ip);

/* ---- transcendentals ---------------------------------------------- */
double exp(double x);
float expf(float x);
double log(double x);
float logf(float x);
double log2(double x);
float log2f(float x);
double log10(double x);
float log10f(float x);
double pow(double x, double y);
float powf(float x, float y);
/* exp2 / exp2f: clang rewrites pow(2.0, x) into exp2(x) (and powf(2.0f, x) into exp2f), so
 * these exist for that lowering as much as for direct callers. Defined as pow(2, x) / powf(2, x)
 * exactly, so the rewrite never changes a result. */
double exp2(double x);
float exp2f(float x);
double sin(double x);
float sinf(float x);
double cos(double x);
float cosf(float x);
double tan(double x);
float tanf(float x);
double asin(double x);
float asinf(float x);
double acos(double x);
float acosf(float x);
double atan(double x);
float atanf(float x);
double atan2(double y, double x);
float atan2f(float y, float x);
double hypot(double x, double y);
float hypotf(float x, float y);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LITHOLIBC_MATH_H */
