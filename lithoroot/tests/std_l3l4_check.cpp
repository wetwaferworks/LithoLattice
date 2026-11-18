// std_l3l4_check.cpp ... LithoRoot std L3 (memory) + L4 (iterator) gate (S3, 2026-11-16).
// run_l3() returns 0 on pass, else the number of the first failed check (Node host).
// The test brings its own bump-arena operator new/delete so it runs without LithoLibC.
#include <iterator>
#include <memory>

static unsigned char g_arena[1 << 16];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) {
  g_top = (g_top + 15u) & ~15u;
  void* p = g_arena + g_top;
  g_top += unsigned(n);
  ++g_live;
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {
int g_dtors = 0;
struct Probe {
  int v;
  explicit Probe(int x) : v(x) {}
  ~Probe() { ++g_dtors; }
};
struct Base { virtual ~Base() = default; int b = 7; };
struct Derived : Base { int d = 9; };
struct Self : std::enable_shared_from_this<Self> { int k = 3; };
struct CountDel { int* n; void operator()(Probe* p) const { ++*n; delete p; } };
struct Vec {  // just enough container for the inserters
  int a[8] = {}; int n = 0;
  using value_type = int; using iterator = int*;
  void push_back(int x) { a[n++] = x; }
  int* begin() { return a; }
  int* end() { return a + n; }
};
}  // namespace

extern "C" int run_l3() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  // unique_ptr: ownership, release/reset, empty deleter costs no space, array form.
  static_assert(sizeof(std::unique_ptr<int>) == sizeof(int*));
  {
    auto u = std::make_unique<Probe>(5);
    CHECK(u && u->v == 5);
    std::unique_ptr<Probe> w = std::move(u);
    CHECK(!u && w->v == 5);
    w.reset(new Probe(6));
    CHECK(g_dtors == 1 && w->v == 6);
  }
  CHECK(g_dtors == 2);
  {
    auto arr = std::make_unique<int[]>(4);
    CHECK(arr[0] == 0 && arr[3] == 0);
    arr[2] = 11;
    CHECK(arr[2] == 11);
    std::unique_ptr<Base> b = std::make_unique<Derived>();
    CHECK(b->b == 7);
    int n = 0;
    { std::unique_ptr<Probe, CountDel> d(new Probe(1), CountDel{&n}); }
    CHECK(n == 1);
  }
  // shared_ptr / weak_ptr counts, aliasing, casts, enable_shared_from_this.
  g_dtors = 0;
  {
    auto s = std::make_shared<Probe>(8);
    CHECK(s.use_count() == 1 && s->v == 8);
    std::weak_ptr<Probe> w = s;
    {
      auto s2 = s;
      CHECK(s.use_count() == 2 && w.use_count() == 2);
      auto l = w.lock();
      CHECK(l && l.use_count() == 3);
    }
    CHECK(s.use_count() == 1);
    s.reset();
    CHECK(g_dtors == 1 && w.expired() && !w.lock());
  }
  {
    std::shared_ptr<Base> b = std::make_shared<Derived>();
    // static_pointer_cast at run time: the Node harness has no __dynamic_cast (that is
    // LithoABI's, linked in the real build); dynamic_pointer_cast is compile-checked below.
    auto d = std::static_pointer_cast<Derived>(b);
    (void)&std::dynamic_pointer_cast<Derived, Base>;
    CHECK(d && d->d == 9 && b.use_count() == 2);
    auto alias = std::shared_ptr<int>(b, &d->d);
    CHECK(*alias == 9 && b.use_count() == 3);
    auto self = std::make_shared<Self>();
    auto again = self->shared_from_this();
    CHECK(again.get() == self.get() && self.use_count() == 2);
    int n = 0;
    { std::shared_ptr<Probe> sd(new Probe(2), CountDel{&n}); auto cp = sd; CHECK(std::get_deleter<CountDel>(sd) != nullptr); }
    CHECK(n == 1);
    std::shared_ptr<Probe> fromU(std::make_unique<Probe>(4));
    CHECK(fromU->v == 4 && fromU.use_count() == 1);
  }
  CHECK(g_live == 0);  // every block freed
  // allocator + uninitialized algorithms + allocator_traits.
  {
    std::allocator<int> a;
    int* p = std::allocator_traits<std::allocator<int>>::allocate(a, 4);
    const int src[4] = {1, 2, 3, 4};
    std::uninitialized_copy(src, src + 4, p);
    CHECK(p[0] == 1 && p[3] == 4);
    std::uninitialized_fill_n(p, 2, 9);
    CHECK(p[1] == 9 && p[2] == 3);
    a.deallocate(p, 4);
    static_assert(std::is_same_v<std::allocator_traits<std::allocator<int>>::rebind_alloc<char>, std::allocator<char>>);
    CHECK(g_live == 0);
  }
  // iterator: traits, distance/next/prev, reverse/move/back_inserter, range access.
  {
    int a[5] = {1, 2, 3, 4, 5};
    static_assert(std::is_same_v<std::iterator_traits<int*>::iterator_category, std::random_access_iterator_tag>);
    CHECK(std::distance(std::begin(a), std::end(a)) == 5);
    CHECK(*std::next(a, 2) == 3 && *std::prev(a + 5) == 5);
    auto r = std::rbegin(a);
    CHECK(*r == 5 && r[1] == 4 && (std::rend(a) - r) == 5);
    Vec v;
    auto bi = std::back_inserter(v);
    for (int x : a) *bi++ = x * 10;
    CHECK(v.n == 5 && v.a[4] == 50 && std::size(a) == 5 && std::ssize(a) == 5);
    auto m = std::make_move_iterator(a + 1);
    CHECK(*m == 2 && std::data(a) == a && !std::empty(a));
  }
  return 0;
#undef CHECK
}
