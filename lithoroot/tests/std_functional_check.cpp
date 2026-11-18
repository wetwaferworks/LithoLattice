// std_functional_check.cpp ... LithoRoot std L4 <functional> gate (S3, 2026-11-16).
// run_fn() returns 0 on pass, else the number of the first failed check (Node host).
// Own bump-arena operator new/delete so it runs without LithoLibC; g_live proves no leaks.
#include <functional>

static unsigned char g_arena[1 << 16];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; return p; }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {
int twice(int x) { return 2 * x; }
struct Obj { int k = 4; int add(int x) const { return k + x; } };
struct Big { long long a[6]; int operator()(int x) const { return int(a[0]) + x; } };
int g_dtor = 0;
struct Tracked { int v; Tracked(int x) : v(x) {} Tracked(const Tracked& o) : v(o.v) {} Tracked(Tracked&& o) noexcept : v(o.v) { o.v = -1; } ~Tracked() { ++g_dtor; } int operator()() const { return v; } };
}  // namespace

extern "C" int run_fn() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  Obj o;
  CHECK(std::invoke(twice, 3) == 6);
  CHECK(std::invoke(&Obj::add, o, 1) == 5);
  CHECK(std::invoke(&Obj::add, &o, 2) == 6);
  CHECK(std::invoke(&Obj::k, o) == 4);
  CHECK(std::invoke(&Obj::add, std::cref(o), 3) == 7);
  int n = 1;
  auto r = std::ref(n);
  r.get() = 9;
  CHECK(n == 9);
  CHECK(std::less<>()(1, 2) && !std::greater<int>()(1, 2) && std::equal_to<>()(3, 3) && std::plus<>()(2, 3) == 5);
  CHECK(std::not_fn([](int x) { return x > 0; })(-1));
  CHECK(std::bind_front(&Obj::add, &o)(10) == 14);
  // hash: ints identity, +0/-0 equal, 64-bit folds the high half.
  CHECK(std::hash<int>()(42) == 42u && std::hash<float>()(0.0f) == std::hash<float>()(-0.0f));
  CHECK(std::hash<unsigned long long>()(1ull << 40) != 0u);
  // function: empty, small (inline), big (heap), copy, move, swap, target.
  {
    std::function<int(int)> f;
    CHECK(!f && f == nullptr);
    f = twice;
    CHECK(f && f(5) == 10 && f.target<int (*)(int)>() != nullptr);
    std::function<int(int)> g = [&o](int x) { return o.k * x; };
    CHECK(g(3) == 12 && g_live == 0);  // one pointer capture: inline, no allocation
    Big b{{100, 0, 0, 0, 0, 0}};
    std::function<int(int)> h = b;
    CHECK(h(1) == 101 && g_live == 1);  // 48 bytes: on the heap
    auto h2 = h;
    CHECK(h2(2) == 102 && g_live == 2);
    auto h3 = std::move(h2);
    CHECK(!h2 && h3(3) == 103 && g_live == 2);
    std::swap(f, h3);
    CHECK(f(4) == 104 && h3(4) == 8);
    std::function<int(int)> m = std::function<int(int)>([](int x) { return x; });
    CHECK(m(7) == 7);
    int (*nullfp)(int) = nullptr;
    std::function<int(int)> e = nullfp;
    CHECK(!e);
  }
  CHECK(g_live == 0);
  {
    g_dtor = 0;
    { std::function<int()> t = Tracked(5); CHECK(t() == 5); auto u = t; CHECK(u() == 5); }
    CHECK(g_dtor >= 3);  // the temporary, the stored copy and the copied function
  }
  std::function fdeduced = [](int a, int b) { return a - b; };
  CHECK(fdeduced(5, 3) == 2);
  return 0;
#undef CHECK
}
