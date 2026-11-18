/* packed vector<bool> checks (S5). Node -O0/-O2 and host libstdc++. Returns 0 on pass. */
#include <vector>
#include <cstddef>
#include <cstdint>
#ifdef __wasm__
static unsigned char g_arena[1 << 20]; static size_t g_top;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void*) noexcept {}
void operator delete[](void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
void operator delete[](void*, size_t) noexcept {}
#endif
extern "C" int run_vb() {
  int fail = 0, t = 0;
  auto ck = [&](bool c) { ++t; if (!c && !fail) fail = t; };
  std::vector<bool> a; for (int i = 0; i < 100; ++i) a.push_back(i % 3 == 0);
  ck(a.size() == 100 && a[0] && !a[1] && a[99] && !a[98]);
  std::vector<bool> b(100, false); for (size_t i = 0; i < b.size(); ++i) b[i] = (i % 3 == 0);
  ck(a == b); b[50] = !b[50]; ck(!(a == b)); b[50].flip(); ck(a == b);
  std::vector<bool> used(5, false); if (!used[2]) used[2] = true; ck(used[2] && !used[1]);
  b.resize(33); ck(b.size() == 33 && b[33 - 1] == (32 % 3 == 0)); b.resize(70, true); ck(b[33] && b[69] && b[30]);
  b.resize(31); b.resize(40); ck(!b[35] && !b[31]);
  int n = 0; for (bool x : a) n += x; ck(n == 34);
  const std::vector<bool>& ca = a; int m = 0; for (auto it = ca.begin(); it != ca.end(); ++it) m += *it; ck(m == 34 && ca.end() - ca.begin() == 100);
  std::vector<bool> c = a; c.pop_back(); ck(c.size() == 99 && c.back() == a[98]); std::vector<bool> d = std::move(c); ck(d.size() == 99 && c.empty());
  std::vector<bool> e{true, false, true}; e.flip(); ck(!e[0] && e[1] && !e[2] && e.size() == 3);
  std::vector<bool> f(32, true), g(31, true); g.push_back(true); ck(f == g);
  return fail;
}
#ifndef __wasm__
#include <cstdio>
int main() { int r = run_vb(); std::printf("host=%d\n", r); return r; }
#endif
