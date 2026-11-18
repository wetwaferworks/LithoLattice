/* rt_int128_check.c ... LithoRoot's in-house __multi3 (src/rt/int128.c).
 *
 * 128-bit products lower to __multi3 on wasm32; with LithoRoot linked and no compiler-rt, this
 * file's multiplies run the in-house body. Edge cases plus 4096 xorshift operand pairs (a quarter
 * with a zero high limb, a quarter with all-ones high limbs) folded into a checksum. Expected
 * values come from Python's exact integers. main() returns the number of failures.
 */
#include <stdint.h>
#include <stdio.h>

typedef unsigned __int128 u128;

static int fails;
static void expect(const char* what, u128 got, uint64_t hi, uint64_t lo) {
  if ((uint64_t)(got >> 64) != hi || (uint64_t)got != lo) {
    printf("FAIL %s: got %016llx%016llx want %016llx%016llx\n", what,
           (unsigned long long)(uint64_t)(got >> 64), (unsigned long long)(uint64_t)got,
           (unsigned long long)hi, (unsigned long long)lo);
    ++fails;
  }
}
static uint64_t xs(uint64_t s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static volatile u128 sink; /* keeps the multiplies out of constant folding */
static u128 mul(u128 a, u128 b) { sink = a; return sink * b; }

int main(void) {
  const u128 all = ~(u128)0, two64 = (u128)1 << 64;
  expect("3*5", mul(3, 5), 0, 0xf);
  expect("2^64*2^64", mul(two64, two64), 0, 0);
  expect("max*max", mul(all, all), 0, 1);
  expect("-1*7", (u128)((__int128)mul((u128)(__int128)-1, 7)), 0xffffffffffffffffull, 0xfffffffffffffff9ull);
  expect("2^127*2", mul((u128)1 << 127, 2), 0, 0);
  expect("(2^64-1)^2", mul(two64 - 1, two64 - 1), 0xfffffffffffffffeull, 1);
  uint64_t s = 0x9E3779B97F4A7C15ull, acc_lo = 0, acc_hi = 0;
  for (int i = 0; i < 4096; ++i) {
    uint64_t a0 = s = xs(s), a1 = s = xs(s), b0 = s = xs(s), b1 = s = xs(s);
    if (i % 4 == 1) a1 = 0;
    if (i % 4 == 2) a1 = b1 = ~0ull;
    u128 r = mul(((u128)a1 << 64) | a0, ((u128)b1 << 64) | b0);
    acc_lo ^= (uint64_t)r;
    acc_hi ^= (uint64_t)(r >> 64);
    acc_lo = (acc_lo << 1) | (acc_lo >> 63);
  }
  expect("4096-pair checksum", ((u128)acc_hi << 64) | acc_lo, 0x2dbce7189167a72bull, 0xe2cd2a377e3c33fdull);
  printf("rt_int128_check: %d failure(s)\n", fails);
  return fails;
}
