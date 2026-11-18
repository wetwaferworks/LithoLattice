/* variant + list checks (S4 tail). Node, -O2 and -O0. Returns 0 on pass. */
#include <variant>
#include <list>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>
#include <cstdlib>

static unsigned char g_arena[8 << 20];
static size_t g_top;
static long g_live;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); ++g_live; return p; }
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

static long g_obj;
struct Tracked { int v; Tracked(int x) : v(x) { ++g_obj; } Tracked(const Tracked& o) : v(o.v) { ++g_obj; } ~Tracked() { --g_obj; } bool operator==(const Tracked& o) const { return v == o.v; } };

struct Json;
using Array = std::vector<Json>;
using Object = std::map<std::string, Json>;
struct Json { std::variant<std::monostate, bool, double, std::int64_t, std::string, Array, Object> v; };

extern "C" int run_vl() {
  int fail = 0;
#define CHECK(c) do { if (!(c)) return 1000 + __LINE__; } while (0)
  {
    using V = std::variant<std::monostate, bool, double, std::int64_t, std::string>;
    V a; CHECK(a.index() == 0 && std::holds_alternative<std::monostate>(a));
    a = true; CHECK(a.index() == 1 && std::get<bool>(a));
    a = 2.5; CHECK(a.index() == 2 && std::get<double>(a) == 2.5);
    a = std::int64_t(5); CHECK(a.index() == 3);
    V b = 7;  // int -> int64_t (double would narrow)
    CHECK(b.index() == 3 && std::get<3>(b) == 7);
    V c = "hi";  // const char* -> string (bool would narrow)
    CHECK(c.index() == 4 && std::get<std::string>(c) == "hi");
    V d = c; CHECK(d == c && !(d == b));
    V e = std::move(d); CHECK(std::get<std::string>(e) == "hi");
    CHECK(std::get_if<std::string>(&e) && !std::get_if<bool>(&e));
    /* get<> on the wrong alternative throws bad_variant_access: not run here (no LithoAbi in this link). */
    int r = std::visit([](const auto& x) -> int { return sizeof(x) > 0 ? 1 : 0; }, e);
    CHECK(r == 1);
    CHECK(b < c && !(c < b));
    e.emplace<std::string>(40, 'x'); CHECK(std::get<4>(e).size() == 40);
  }
  {
    std::variant<int, Tracked> t = Tracked(3);
    CHECK(g_obj == 1);
    t = 4; CHECK(g_obj == 0 && std::get<int>(t) == 4);
    t = Tracked(9); CHECK(g_obj == 1);
    { auto u = t; CHECK(g_obj == 2 && std::get<Tracked>(u).v == 9); }
    CHECK(g_obj == 1);
  }
  CHECK(g_obj == 0);
  /* valueless_by_exception after a throwing emplace: needs LithoAbi in the link; not run here. */
  {
    Json j; j.v = Array{}; std::get<Array>(j.v).push_back(Json{std::string("k")});
    CHECK(std::get<Array>(j.v).size() == 1);
    Json o; o.v = Object{}; std::get<Object>(o.v)["a"] = j; CHECK(std::get<Object>(o.v).count("a"));
  }
  /* list: random ops vs a vector model; references stay put. */
  {
    std::list<int> l; std::vector<int> m;
    unsigned s = 99;
    auto rnd = [&] { s = s * 1103515245u + 12345u; return s >> 8; };
    l.push_back(-1); m.push_back(-1);
    const int* pin = &l.front();
    for (int i = 0; i < 4000; ++i) {
      unsigned op = rnd() % 6;
      if (op == 0) { l.push_back(i); m.push_back(i); }
      else if (op == 1) { l.push_front(i); m.insert(m.begin(), i); }
      else if (op == 2 && m.size() > 1 && m.back() != -1) { l.pop_back(); m.pop_back(); }
      else if (op == 3 && m.size() > 1) {
        size_t k = rnd() % m.size(); if (m[k] == -1) continue;
        auto it = l.begin(); for (size_t q = 0; q < k; ++q) ++it; l.erase(it); m.erase(m.begin() + k);
      } else if (op == 4 && !m.empty()) {  // LRU touch: splice element k to front
        size_t k = rnd() % m.size();
        auto it = l.begin(); for (size_t q = 0; q < k; ++q) ++it;
        l.splice(l.begin(), l, it); int x = m[k]; m.erase(m.begin() + k); m.insert(m.begin(), x);
      } else { l.push_back(i); m.push_back(i); }
      if (l.size() != m.size()) return 2;
    }
    size_t q = 0; for (int x : l) if (x != m[q++]) return 3;
    q = m.size(); for (auto it = l.rbegin(); it != l.rend(); ++it) if (*it != m[--q]) return 4;
    if (*pin != -1) return 5;
    bool found = false; for (auto& x : l) if (&x == pin) found = true;
    if (!found) return 6;
    l.sort(); std::sort(m.begin(), m.end());
    q = 0; for (int x : l) if (x != m[q++]) return 7;
    l.reverse(); q = m.size(); for (int x : l) if (x != m[--q]) return 8;
    std::list<int> o{1, 2, 3}; l.splice(l.end(), o); if (!o.empty() || l.size() != m.size() + 3) return 9;
    l.remove_if([](int x) { return x % 2 == 0; });
    for (int x : l) if (x % 2 == 0) return 10;
  }
  /* sort stability */
  {
    struct P { int k, ord; };
    std::list<P> l; unsigned s = 5;
    for (int i = 0; i < 500; ++i) { s = s * 69069u + 1; l.push_back({int((s >> 16) % 10), i}); }
    l.sort([](const P& a, const P& b) { return a.k < b.k; });
    const P* prev = nullptr;
    for (auto& p : l) { if (prev && (prev->k > p.k || (prev->k == p.k && prev->ord > p.ord))) return 11; prev = &p; }
  }
  CHECK(g_live == 0);
  return fail;
}
