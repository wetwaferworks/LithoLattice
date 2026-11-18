// std_algorithm_check.cpp ... LithoRoot std L4 <algorithm> gate (S3, 2026-11-16).
// run_algo() returns 0 on pass, else the number of the first failed check (Node host).
// Randomised against brute-force references: sort/stable_sort/nth_element/partial_sort on
// sizes 0..300 with few/many duplicates, sorted, reversed and organ-pipe inputs.
#include <algorithm>

static unsigned char g_arena[1 << 18];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; return p; }
void* operator new(std::size_t n) { return operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {
unsigned g_seed = 12345u;
unsigned rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
struct Gen { using result_type = unsigned; static constexpr unsigned min() { return 0; } static constexpr unsigned max() { return 0xffffff; } unsigned operator()() { return rnd(); } };
struct KV { int k, v; };
int a[300], b[300];
KV s[300], t[300];
void fill_case(int n, int kind) {
  for (int i = 0; i < n; ++i) {
    switch (kind) {
      case 0: a[i] = int(rnd() % 1000); break;
      case 1: a[i] = int(rnd() % 4); break;
      case 2: a[i] = i; break;
      case 3: a[i] = n - i; break;
      case 4: a[i] = i < n / 2 ? i : n - i; break;
      default: a[i] = 7; break;
    }
  }
}
void ref_sort(int* p, int n) { for (int i = 1; i < n; ++i) for (int j = i; j > 0 && p[j] < p[j - 1]; --j) std::swap(p[j], p[j - 1]); }
}  // namespace

extern "C" int run_algo() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  CHECK(std::max(1, 2) == 2 && std::min({5, 3, 9}) == 3 && std::clamp(12, 0, 10) == 10 && std::clamp(-1, 0, 10) == 0);
  for (int kind = 0; kind < 6; ++kind)
    for (int n = 0; n <= 300; n += (n < 40 ? 1 : 37)) {
      fill_case(n, kind);
      for (int i = 0; i < n; ++i) b[i] = a[i];
      ref_sort(b, n);
      int w[300];
      std::copy(a, a + n, w); std::sort(w, w + n);
      if (!std::equal(w, w + n, b)) return 1000 + kind * 1000 + n;
      std::copy(a, a + n, w); std::stable_sort(w, w + n);
      if (!std::equal(w, w + n, b)) return 100000 + kind * 1000 + n;
      if (n > 0) {
        int k = int(rnd() % unsigned(n));
        std::copy(a, a + n, w); std::nth_element(w, w + k, w + n);
        if (w[k] != b[k]) return 200000 + kind * 1000 + n;
        for (int i = 0; i < k; ++i) if (b[k] < w[i]) return 300000 + kind * 1000 + n;
        for (int i = k + 1; i < n; ++i) if (w[i] < b[k]) return 400000 + kind * 1000 + n;
        std::copy(a, a + n, w); std::partial_sort(w, w + k, w + n);
        if (!std::equal(w, w + k, b)) return 500000 + kind * 1000 + n;
      }
      std::copy(a, a + n, w); std::make_heap(w, w + n);
      if (!std::is_heap(w, w + n)) return 600000 + kind * 1000 + n;
      std::sort_heap(w, w + n);
      if (!std::equal(w, w + n, b)) return 700000 + kind * 1000 + n;
    }
  ++c;
  // stable_sort keeps equal keys in input order (buffer and in-place fallback paths).
  for (int n = 0; n < 300; n += 13) {
    for (int i = 0; i < n; ++i) s[i] = KV{int(rnd() % 5), i};
    std::copy(s, s + n, t);
    auto byk = [](const KV& x, const KV& y) { return x.k < y.k; };
    std::stable_sort(t, t + n, byk);
    for (int i = 1; i < n; ++i) if (t[i - 1].k > t[i].k || (t[i - 1].k == t[i].k && t[i - 1].v > t[i].v)) return 800000 + n;
    std::copy(s, s + n, t);
    std::__lk::merge_sort_inplace(t, t + n, byk);
    for (int i = 1; i < n; ++i) if (t[i - 1].k > t[i].k || (t[i - 1].k == t[i].k && t[i - 1].v > t[i].v)) return 900000 + n;
  }
  CHECK(g_live == 0);
  // search / bounds / modifying.
  int v[10] = {1, 2, 2, 3, 5, 5, 5, 8, 9, 9};
  CHECK(std::lower_bound(v, v + 10, 5) - v == 4 && std::upper_bound(v, v + 10, 5) - v == 7 && std::binary_search(v, v + 10, 8));
  CHECK(std::find(v, v + 10, 3) - v == 3 && std::count(v, v + 10, 5) == 3 && std::count_if(v, v + 10, [](int x) { return x > 4; }) == 6);
  int u[10]; std::copy(v, v + 10, u);
  CHECK(std::unique(u, u + 10) - u == 6 && u[5] == 9);
  std::copy(v, v + 10, u);
  CHECK(std::remove(u, u + 10, 5) - u == 7 && u[4] == 8);
  std::copy(v, v + 10, u); std::reverse(u, u + 10);
  CHECK(u[0] == 9 && u[9] == 1);
  int r[5] = {1, 2, 3, 4, 5};
  CHECK(std::rotate(r, r + 2, r + 5) == r + 3 && r[0] == 3 && r[4] == 2);
  int p1[4] = {1, 2, 3, 4}, p2[4] = {4, 1, 3, 2}, p3[4] = {4, 1, 3, 3};
  CHECK(std::is_permutation(p1, p1 + 4, p2) && !std::is_permutation(p1, p1 + 4, p3));
  int x1[3] = {1, 3, 5}, x2[3] = {2, 3, 6}, out[6];
  CHECK(std::set_union(x1, x1 + 3, x2, x2 + 3, out) - out == 5 && out[4] == 6);
  int sh[50]; for (int i = 0; i < 50; ++i) sh[i] = i;
  std::shuffle(sh, sh + 50, Gen{});
  int chk[50]; std::copy(sh, sh + 50, chk); std::sort(chk, chk + 50);
  bool ident = true; for (int i = 0; i < 50; ++i) { if (chk[i] != i) return 990000 + i; if (sh[i] != i) ident = false; }
  CHECK(!ident);
  CHECK(std::all_of(v, v + 10, [](int x) { return x > 0; }) && std::none_of(v, v + 10, [](int x) { return x > 9; }));
  CHECK(*std::min_element(p2, p2 + 4) == 1 && *std::max_element(p2, p2 + 4) == 4);
  CHECK(std::lexicographical_compare(x1, x1 + 3, x2, x2 + 3));
  return 0;
#undef CHECK
}
