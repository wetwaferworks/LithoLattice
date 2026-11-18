// std_string_check.cpp ... LithoRoot std L5 <string> gate (S3, 2026-11-16).
// run_str() returns 0 on pass (Node host). Own arena new/delete; g_live checks no leaks.
// sto*/to_string(double) need LithoLibC's strto*/snprintf: compile-checked here, run in S5+.
#include <string>

static unsigned char g_arena[1 << 18];
static unsigned g_top = 0;
static int g_live = 0;
void* operator new(std::size_t n) { g_top = (g_top + 15u) & ~15u; void* p = g_arena + g_top; g_top += unsigned(n); ++g_live; return p; }
void operator delete(void* p) noexcept { if (p) --g_live; }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

extern "C" int run_str() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  using namespace std::literals;
  {
    std::string e;
    CHECK(e.empty() && e.c_str()[0] == 0 && e.capacity() == 15 && sizeof(std::string) == 24);
    std::string s = "short";
    CHECK(s.size() == 5 && g_live == 0);  // SSO: no allocation
    std::string l = "this one is longer than fifteen";
    CHECK(l.size() == 31 && g_live == 1 && l.c_str()[31] == 0);
    std::string m = std::move(l);
    CHECK(l.empty() && m.size() == 31 && g_live == 1);
    std::string sm = std::move(s);
    CHECK(sm == "short" && s.empty());
    m += "!";
    m.append(3, 'x').push_back('y');
    CHECK(m.ends_with("!xxxy") && m.size() == 36);
    m.append(m.data(), 4);  // self-append across a reallocation
    CHECK(m.ends_with("this") && m.size() == 40);
    m.erase(0, 5);
    CHECK(m.starts_with("one is"));
    m.replace(0, 3, "ONE");
    CHECK(m.starts_with("ONE is") && m.find("longer") == 7 && m.rfind('x') == 29);
    m.replace(0, 3, m.c_str() + 4, 2);  // self-aliasing replace
    CHECK(m.starts_with("is is"));
    m.insert(0, "<<").insert(m.size(), ">>");
    CHECK(m.front() == '<' && m.back() == '>');
    auto t = m.substr(2, 5);
    CHECK(t == "is is" && t.compare("is it") < 0);
    std::string cat = "a"s + "b" + 'c' + std::string("d") + "e";
    CHECK(cat == "abcde");
    cat.resize(2);
    CHECK(cat == "ab" && cat.size() == 2);
    cat.resize(5, 'z');
    CHECK(cat == "abzzz");
    std::string a1(40, 'q');
    a1.clear(); a1.shrink_to_fit();
    CHECK(a1.capacity() == 15);
    std::string sw1 = "small", sw2(30, 'L');
    sw1.swap(sw2);
    CHECK(sw1.size() == 30 && sw2 == "small");
    std::string_view v = sw2;
    CHECK(v == "small" && std::string(v) == sw2 && sw2 < sw1 == false);
    std::string it(v.begin(), v.end());
    CHECK(it == sw2 && std::hash<std::string>()(it) == std::hash<std::string_view>()("small"));
    CHECK(std::to_string(-42) == "-42" && std::to_string(4000000000u) == "4000000000");
    std::string er = "a-b-c";
    CHECK(std::erase(er, '-') == 2 && er == "abc");
    std::string cp = er;
    CHECK(cp == er && cp.data() != er.data());
  }
  CHECK(g_live == 0);
  (void)&std::stoi; (void)&std::stod;
  return 0;
#undef CHECK
}
