// std_unordered_check.cpp ... LithoRoot std L6 gate, part 2 (S4, 2026-11-16):
// unordered_map / unordered_set randomised against a presence table, plus reference
// stability across rehash. run_unord() returns 0 on pass (Node host).
#include <string>
#include <unordered_map>
#include <unordered_set>

static unsigned char g_arena[1 << 22];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
namespace { unsigned g_seed = 99u; unsigned rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; } }

extern "C" int run_unord() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  {
    std::unordered_map<int, int> m;
    std::unordered_set<int> s;
    bool ref[500] = {};
    int refv[500] = {};
    for (int step = 0; step < 5000; ++step) {
      const int k = int(rnd() % 500);
      switch (rnd() % 4) {
        case 0: case 1: {
          auto r = m.try_emplace(k, step); auto r2 = s.insert(k);
          if (r.second != !ref[k] || r2.second != !ref[k]) return 1000 + step;
          if (!ref[k]) refv[k] = step;
          ref[k] = true; break;
        }
        case 2: if (m.erase(k) != size_t(ref[k]) || s.erase(k) != size_t(ref[k])) return 2000 + step; ref[k] = false; break;
        default: { auto it = m.find(k); if ((it != m.end()) != ref[k] || (ref[k] && it->second != refv[k])) return 3000 + step; }
      }
      if (m.load_factor() > m.max_load_factor()) return 4000 + step;
    }
    size_t n = 0, live = 0;
    for (auto& [k, v] : m) { if (!ref[k] || v != refv[k]) return 5000 + k; ++n; }
    for (bool b : ref) live += b;
    CHECK(n == live && n == m.size() && n == s.size());
    for (int k : s) if (!ref[k]) return 6000 + k;
    int* stable = &m.try_emplace(10000, 1).first->second;
    m.reserve(4096);
    CHECK(*stable == 1 && &m[10000] == stable && m.bucket_count() >= 4096);  // node stability across rehash
    auto cp = m;
    CHECK(cp == m && cp.size() == m.size());
    CHECK(std::erase_if(m, [](auto& kv) { return kv.first % 3 == 0; }) > 0);
    for (auto& kv : m) if (kv.first % 3 == 0) return 7000;
    std::unordered_map<std::string, int> sm{{"x", 1}, {"y", 2}};
    sm["z"] = 3;
    CHECK(sm.at("y") == 2 && sm.size() == 3 && sm.contains("z") && !sm.contains("w"));
    m.erase(m.begin(), m.end());
    CHECK(m.empty());
  }
  CHECK(g_live == 0);
  return 0;
#undef CHECK
}
