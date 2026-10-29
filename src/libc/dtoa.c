/* dtoa.c ... the exact decimal expansion behind printf's float
 * conversions. See litholibc_internal.h for the contract.
 *
 * Method: a finite double is m x 2^e2 with m a 53-bit (or smaller,
 * for subnormals) integer. If e2 >= 0 the value is the integer
 * m << e2 and its decimal digits are that integer's digits. If
 * e2 < 0, write k = -e2 and use m / 2^k = (m x 5^k) / 10^k: the
 * digits are those of the integer m x 5^k, with the decimal point
 * shifted k places. Either way the whole expansion is one bigint
 * and one decimal conversion... exact by construction, with no
 * iterative approximation anywhere, which is what makes the
 * printf float contract (correct rounding at any precision up to
 * the full expansion) provable rather than hoped for.
 */

#include <stdint.h>
#include <string.h>

#include "litholibc_bigint.h"
#include "litholibc_internal.h"

int litholibc_dtoa_digits(double v, char* digits, int* dec_exp, int* sign) {
  uint64_t bits, frac, m;
  int biased, e2, nd;
  litho_big_t p;
  char raw[LITHO_BIG_MAX_DIGITS + 1];

  memcpy(&bits, &v, sizeof bits);
  *sign = (int)(bits >> 63);
  biased = (int)((bits >> 52) & 0x7ffu);
  frac = bits & ((UINT64_C(1) << 52) - 1);
  if (biased == 0) {
    m = frac;
    e2 = -1074;
  } else {
    m = frac | (UINT64_C(1) << 52);
    e2 = biased - 1023 - 52;
  }

  litho_big_from_u64(&p, m);
  if (e2 >= 0) {
    litho_big_shl(&p, e2);
    nd = litho_big_to_digits(&p, raw);
    *dec_exp = nd;
  } else {
    int k = -e2;
    /* Cannot overflow capacity: m < 2^53 and 5^1074 < 2^2493,
     * so the product stays under 2^2546 of the 2^5120 space. */
    (void)litho_big_mul_pow5(&p, k);
    nd = litho_big_to_digits(&p, raw);
    *dec_exp = nd - k;
  }

  /* Trim trailing zeros; dec_exp is anchored at the first digit
   * and does not move. Keep at least one digit. */
  while (nd > 1 && raw[nd - 1] == '0') nd--;
  memcpy(digits, raw, (size_t)nd);
  digits[nd] = '\0';
  return nd;
}
