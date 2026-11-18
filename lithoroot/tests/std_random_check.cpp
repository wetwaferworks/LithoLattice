/* <random> subset checks (S5): engine sequences equal the standard's (10000th-value
 * checks from [rand.predef]) and distributions stay in range and hit every value. */
#include <random>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstddef>
#ifdef __wasm__
static unsigned char g_arena[1 << 20]; static size_t g_top;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void*) noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
void operator delete[](void*, size_t) noexcept {}
#endif
template <class E> static unsigned long long nth10000(E e) { for (int i = 1; i < 10000; ++i) e(); return e(); }
extern "C" int run_rand() {
  int fail = 0, t = 0;
  auto ck = [&](bool c) { ++t; if (!c && !fail) fail = t; };
  ck(nth10000(std::mt19937{}) == 4123659995ull);
  ck(nth10000(std::mt19937_64{}) == 9981545732273789042ull);
  ck(nth10000(std::minstd_rand0{}) == 1043618065ull);
  ck(nth10000(std::minstd_rand{}) == 399268537ull);
  std::mt19937 a(0xF00D), b(0xF00D); ck(a() == b() && a == b);
  std::mt19937 g(7); std::uniform_int_distribution<int> d(-3, 4); int seen[8] = {}; bool in = true;
  for (int i = 0; i < 4000; ++i) { int v = d(g); in = in && v >= -3 && v <= 4; if (v >= -3 && v <= 4) ++seen[v + 3]; }
  bool all = true; for (int s : seen) all = all && s > 350; ck(in && all);
  std::uniform_int_distribution<uint64_t> big; uint64_t hi = 0; for (int i = 0; i < 200; ++i) hi |= big(g); ck(hi >> 63);
  std::uniform_real_distribution<double> u(2.0, 3.0); bool ur = true; double mn = 9, mx = 0;
  for (int i = 0; i < 4000; ++i) { double v = u(g); ur = ur && v >= 2.0 && v < 3.0; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
  ck(ur && mn < 2.01 && mx > 2.99);
  std::uniform_real_distribution<float> uf; bool fr = true; for (int i = 0; i < 4000; ++i) { float v = uf(g); fr = fr && v >= 0.f && v < 1.f; } ck(fr);
  std::vector<int> c{1, 2, 3, 4, 5, 6}; std::mt19937 sh(0xF00D); std::shuffle(c.begin(), c.end(), sh);
  std::vector<int> s = c; std::sort(s.begin(), s.end()); ck(s == std::vector<int>({1, 2, 3, 4, 5, 6}));
  return fail;
}
#ifndef __wasm__
#include <cstdio>
int main() { int r = run_rand(); std::printf("host=%d\n", r); return r; }
#endif
