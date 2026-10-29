/* litholibc_bigint.h ... internal exact-arithmetic helper for the
 * stdio / stdlib / math piece. NOT a public header.
 *
 * A fixed-capacity unsigned big integer, little-endian 32-bit
 * words, used by exactly two consumers:
 *
 *   - the printf digit engine (dtoa.c), which expands a double to
 *     its EXACT decimal digit string (value = mantissa x 5^k for
 *     negative binary exponents, so the decimal digits are just
 *     the digits of one big integer), and
 *   - strtod (strtod.c), which parses a decimal string into a big
 *     integer and rounds it into a double by exact long division.
 *
 * Capacity: 160 words = 5120 bits. The worst cases of both
 * consumers: a subnormal double expands to m x 5^1074 (about 2546
 * bits); strtod keeps 800 significant decimal digits (about 2657
 * bits) and shifts them by up to 1074 for the subnormal rounding
 * path (about 3731 bits). 5120 covers both with headroom, and the
 * operations that could overflow say so (mul_small reports it)
 * instead of wrapping silently.
 */
#ifndef LITHOLIBC_BIGINT_H
#define LITHOLIBC_BIGINT_H

#include <stddef.h>
#include <stdint.h>

#define LITHO_BIG_WORDS 160

typedef struct {
  uint32_t w[LITHO_BIG_WORDS];
  int n; /* words in use, 0 means the value zero */
} litho_big_t;

void litho_big_zero(litho_big_t* a);
void litho_big_from_u64(litho_big_t* a, uint64_t v);
int litho_big_is_zero(const litho_big_t* a);
int litho_big_bitlen(const litho_big_t* a);          /* floor(log2(a)) + 1, 0 for zero */
int litho_big_test_bit(const litho_big_t* a, int i); /* bit i, 0 beyond the top */
int litho_big_cmp(const litho_big_t* a, const litho_big_t* b);
/* a -= b; requires a >= b. */
void litho_big_sub(litho_big_t* a, const litho_big_t* b);
/* a *= m. Returns 0 on success, -1 if the product needed more
 * than the capacity (a is then unspecified). */
int litho_big_mul_small(litho_big_t* a, uint32_t m);
/* a *= 5^e and a *= 10^e, chunked. Return as mul_small. */
int litho_big_mul_pow5(litho_big_t* a, int e);
int litho_big_mul_pow10(litho_big_t* a, int e);
/* a <<= bits / a >>= bits, in place. Left shift past capacity
 * drops high bits (callers size their shifts); right shift is a
 * floor. */
void litho_big_shl(litho_big_t* a, int bits);
void litho_big_shr(litho_big_t* a, int bits);
/* a mod m for a single-word-ish m (m < 2^32), and a /= m returning
 * the remainder. Used by the decimal digit conversion. */
uint32_t litho_big_divmod_small(litho_big_t* a, uint32_t m);
/* Full division: q = num / den, r = num mod den (den nonzero).
 * Bit-by-bit long division; q and r may not alias num or den. */
void litho_big_divmod(const litho_big_t* num, const litho_big_t* den,
                      litho_big_t* q, litho_big_t* r);
/* Convert to a decimal digit string (no leading zeros, "0" for
 * zero). out must hold at least LITHO_BIG_MAX_DIGITS + 1 bytes.
 * Returns the digit count. */
#define LITHO_BIG_MAX_DIGITS 1545 /* floor(5120 * log10(2)) + 1 */
int litho_big_to_digits(const litho_big_t* a, char* out);

#endif /* LITHOLIBC_BIGINT_H */
