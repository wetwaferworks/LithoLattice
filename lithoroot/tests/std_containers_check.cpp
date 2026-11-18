// std_containers_check.cpp ... LithoRoot std L6/L8 gate, part 1 (S4, 2026-11-16): vector,
// array, optional, span, map/set (randomised against a brute-force reference with the
// red-black invariants checked after every step), atomic (single thread: values and
// orders, never a wait that could block). run_cont() returns 0 on pass (Node host).
#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

static unsigned char g_arena[1 << 22];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {
unsigned g_seed = 777u;
unsigned rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
int g_objs = 0;
struct Obj {
  int v;
  Obj(int x = 0) : v(x) { ++g_objs; }
  Obj(const Obj& o) : v(o.v) { ++g_objs; }
  Obj(Obj&& o) noexcept : v(o.v) { ++g_objs; }
  Obj& operator=(const Obj&) = default;
  Obj& operator=(Obj&&) = default;
  ~Obj() { --g_objs; }
  bool operator==(const Obj& o) const { return v == o.v; }
};
}  // namespace

extern "C" int run_cont() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  // vector
  {
    std::vector<Obj> v;
    for (int i = 0; i < 100; ++i) v.push_back(Obj(i));
    CHECK(v.size() == 100 && v[57].v == 57 && v.capacity() >= 100 && g_objs == 100);
    v.insert(v.begin() + 10, Obj(-1));
    CHECK(v[10].v == -1 && v[11].v == 10 && v.size() == 101);
    v.erase(v.begin(), v.begin() + 11);
    CHECK(v.front().v == 10 && v.size() == 90 && g_objs == 90);
    v.emplace_back(v[0]);  // aliasing an element across growth
    CHECK(v.back().v == 10);
    v.insert(v.begin(), v.begin() + 1, v.begin() + 4);  // range from itself
    CHECK(v[0].v == 11 && v[2].v == 13 && v[3].v == 10);
    v.resize(5); CHECK(v.size() == 5 && g_objs == 5);
    v.resize(8, Obj(9)); CHECK(v[7].v == 9 && g_objs == 8);
    std::vector<Obj> w = v; CHECK(w == v && g_objs == 16);
    std::vector<Obj> m = std::move(w); CHECK(w.empty() && m.size() == 8);
    CHECK(std::erase_if(m, [](const Obj& o) { return o.v == 9; }) == 3 && m.size() == 5);
    v.clear(); v.shrink_to_fit(); CHECK(v.capacity() == 0);
    std::vector<int> a{3, 1, 2}; std::vector<int> b{3, 1, 3};
    CHECK(a < b && (a <=> b) < 0 && a != b);
    std::vector<std::string> vs(3, "abc"); vs.insert(vs.begin() + 1, "zzz");
    CHECK(vs[1] == "zzz" && vs[3] == "abc");
  }
  CHECK(g_objs == 0 && g_live == 0);
  // array / span / optional
  {
    std::array<int, 4> a{4, 3, 2, 1};
    auto [x, y, z, w] = a;
    CHECK(x == 4 && w == 1 && a.size() == 4 && std::get<2>(a) == 2);
    std::span<int> s = a;
    s[0] = 40;
    CHECK(a[0] == 40 && s.subspan(1, 2).size() == 2 && s.last(1)[0] == 1 && std::as_bytes(s).size() == 16);
    std::span<const int, 4> fs = a;
    CHECK(fs.first<2>()[1] == 3);
    std::optional<std::string> o;
    CHECK(!o && o.value_or("d") == "d");
    o = "hello";
    CHECK(o && *o == "hello" && o->size() == 5 && o == std::string("hello"));
    std::optional<std::string> o2 = o; o.reset();
    CHECK(!o && o2 == "hello" && o != o2);
    std::optional<int> oi = 5;
    CHECK(oi.transform([](int q) { return q * 2; }) == 10 && std::make_optional(3) < oi);
  }
  CHECK(g_live == 0);
  // map/set randomised: 3000 ops on keys 0..199 vs a presence table.
  {
    std::map<int, int> mp;
    std::set<int> st;
    bool ref[200] = {};
    int refv[200] = {};
    for (int step = 0; step < 3000; ++step) {
      const int k = int(rnd() % 200);
      switch (rnd() % 4) {
        case 0: case 1: {
          auto r = mp.try_emplace(k, step);
          auto r2 = st.insert(k);
          if (r.second != !ref[k] || r2.second != !ref[k]) return 1000 + step;
          if (!ref[k]) refv[k] = step;
          ref[k] = true;
          break;
        }
        case 2: {
          const size_t n = mp.erase(k), n2 = st.erase(k);
          if (n != size_t(ref[k]) || n2 != n) return 2000 + step;
          ref[k] = false;
          break;
        }
        default: {
          auto it = mp.find(k);
          if ((it != mp.end()) != ref[k] || (ref[k] && it->second != refv[k])) return 3000 + step;
        }
      }
      if (mp.__verify() < 0 || st.__verify() < 0) return 4000 + step;
    }
    int prev = -1; size_t n = 0;
    for (auto& [k, v] : mp) { if (k <= prev || !ref[k] || v != refv[k]) return 5000 + k; prev = k; ++n; }
    CHECK(n == mp.size() && n == st.size());
    int back = 1000;
    for (auto it = mp.end(); it != mp.begin();) { --it; if (it->first >= back) return 6000; back = it->first; }
    CHECK(std::equal(st.begin(), st.end(), mp.begin(), [](int a, const std::pair<const int, int>& b) { return a == b.first; }));
    auto lb = mp.lower_bound(100);
    CHECK(lb == mp.end() || lb->first >= 100);
    std::map<int, int> cp = mp;
    CHECK(cp == mp && cp.__verify() >= 0);
    mp[1000] = 5; mp[1000] += 1;
    CHECK(mp.at(1000) == 6 && mp.contains(1000) && cp != mp);
    CHECK(std::erase_if(mp, [](auto& kv) { return kv.first % 2 == 0; }) > 0 && mp.__verify() >= 0);
    for (auto& [k, v] : mp) if (k % 2 == 0) return 7000;
    std::map<std::string, int> sm{{"b", 2}, {"a", 1}, {"c", 3}};
    CHECK(sm.begin()->first == "a" && sm["c"] == 3 && sm.size() == 3);
    mp.clear(); cp.clear(); st.clear(); sm.clear();
  }
  CHECK(g_live == 0);
  // atomic (single thread)
  {
    std::atomic<unsigned long long> a{1};
    CHECK(a.is_lock_free() && std::atomic<unsigned long long>::is_always_lock_free && alignof(decltype(a)) == 8);
    CHECK(a.fetch_add(41, std::memory_order_relaxed) == 1 && a.load() == 42);
    unsigned long long e = 41;
    CHECK(!a.compare_exchange_strong(e, 7) && e == 42 && a.compare_exchange_strong(e, 7) && a == 7ull);
    CHECK(a.exchange(9) == 7 && (a |= 6) == 15 && (a &= 3) == 3 && ++a == 4);
    std::atomic<double> d{1.5};
    CHECK(d.fetch_add(2.25) == 1.5 && d.load() == 3.75);
    int arr[3] = {};
    std::atomic<int*> p{arr};
    CHECK(p.fetch_add(2) == arr && p.load() == arr + 2);
    std::atomic<bool> b{false};
    CHECK(!b.exchange(true) && b.load());
    std::atomic_flag f;
    CHECK(!f.test_and_set() && f.test());
    a.wait(5);  // value differs: returns at once, never blocks
    std::atomic_thread_fence(std::memory_order_seq_cst);
    CHECK(std::atomic_load_explicit(&a, std::memory_order_acquire) == 4);
  }
  return 0;
#undef CHECK
}
