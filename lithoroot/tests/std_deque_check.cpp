// std_deque_check.cpp ... LithoRoot std L6 gate, part 3 (S4, 2026-11-16): deque (randomised
// against a vector reference, reference stability at both ends), queue, priority_queue, stack.
#include <deque>
#include <queue>
#include <stack>
#include <vector>
static unsigned char g_arena[1 << 22];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
namespace { unsigned g_seed = 5u; unsigned rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; } }
extern "C" int run_deque() {
  {
    std::deque<int> d;
    std::vector<int> ref;
    d.push_back(-7);
    int* anchor = &d.front();
    ref.push_back(-7);
    for (int step = 0; step < 6000; ++step) {
      const unsigned op = rnd() % 8;
      const int v = int(rnd() % 1000);
      if (op < 3) { d.push_back(v); ref.push_back(v); }
      else if (op < 6) { d.push_front(v); ref.insert(ref.begin(), v); }
      else if (op == 6 && d.size() > 1 && &d.front() != anchor) { d.pop_front(); ref.erase(ref.begin()); }
      else if (op == 7 && d.size() > 1 && &d.back() != anchor) { d.pop_back(); ref.pop_back(); }
      if (d.size() != ref.size()) return 1000 + step;
    }
    if (*anchor != -7) return 2;  // never moved across thousands of end pushes
    for (size_t i = 0; i < ref.size(); ++i) if (d[i] != ref[i]) return 3;
    d.erase(d.begin() + 3, d.begin() + 10); ref.erase(ref.begin() + 3, ref.begin() + 10);
    d.insert(d.begin() + 5, 4242); ref.insert(ref.begin() + 5, 4242);
    d.erase(d.end() - 20, d.end() - 2); ref.erase(ref.end() - 20, ref.end() - 2);
    if (d.size() != ref.size()) return 4;
    for (size_t i = 0; i < ref.size(); ++i) if (d[i] != ref[i]) return 5;
    std::deque<int> cp = d;
    if (!(cp == d)) return 6;
    std::queue<int> q; for (int i = 0; i < 5; ++i) q.push(i); q.pop();
    if (q.front() != 1 || q.back() != 4 || q.size() != 4) return 7;
    std::priority_queue<int> pq; for (int x : {5, 1, 9, 3}) pq.push(x);
    if (pq.top() != 9) return 8; pq.pop(); if (pq.top() != 5) return 9;
    std::stack<int> s; s.push(1); s.push(2); if (s.top() != 2) return 10; s.pop(); if (s.top() != 1) return 11;
  }
  if (g_live != 0) return 12;
  return 0;
}
