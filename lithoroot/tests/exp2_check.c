/* exp2_check.c ... LithoLibC exp2 / exp2f are pow(2, x) / powf(2, x) bit for bit.
 *
 * clang rewrites pow(2.0, x) into exp2(x); the check pins that the rewrite cannot change a result.
 * Specials (nan, +-inf, +-0, overflow, underflow) plus 20000 xorshift inputs across [-1100, 1100]
 * and a dense band in [-4, 4]. A volatile base keeps the reference calls as real pow calls (a
 * literal 2.0 would be rewritten into exp2 itself). main() returns the number of failures.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile double two = 2.0;
static volatile float twof = 2.0f;
static int fails;
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint32_t bitsf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void one(double x) {
  double a = exp2(x), b = pow(two, x);
  float af = exp2f((float)x), bf = powf(twof, (float)x);
  if (bits(a) != bits(b) && !(isnan(a) && isnan(b))) { if (fails < 8) printf("FAIL exp2(%a)=%a pow=%a\n", x, a, b); ++fails; }
  if (bitsf(af) != bitsf(bf) && !(isnan(af) && isnan(bf))) { if (fails < 8) printf("FAIL exp2f(%a)=%a powf=%a\n", x, (double)af, (double)bf); ++fails; }
}
int main(void) {
  const double sp[] = {0.0, -0.0, 1.0, -1.0, 0.5, 10.0, 1023.0, 1024.0, -1022.0, -1074.0, -1075.0, 1e300, -1e300, NAN, INFINITY, -INFINITY};
  for (unsigned i = 0; i < sizeof sp / sizeof sp[0]; ++i) one(sp[i]);
  uint64_t s = 0x243F6A8885A308D3ull;
  for (int i = 0; i < 20000; ++i) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    double u = (double)(s >> 11) / 9007199254740992.0;
    one(i & 1 ? (u * 2200.0 - 1100.0) : (u * 8.0 - 4.0));
  }
  if (exp2(3.0) != 8.0 || exp2(-1.0) != 0.5 || exp2f(10.0f) != 1024.0f) { printf("FAIL exact powers\n"); ++fails; }
  printf("exp2_check: %d failure(s)\n", fails);
  return fails;
}
