/* format subset checks (S4 tail). Node, -O2 and -O0. Returns 0 on pass. */
#include <format>
#include <string>
#include <cstdint>
static unsigned char g_arena[1 << 20];
static size_t g_top;
void* operator new(size_t n) { n = (n + 15) & ~size_t(15); void* p = g_arena + g_top; g_top += n; if (g_top > sizeof g_arena) __builtin_trap(); return p; }
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
static int eq(const std::string& a, const char* b) { return a == b; }
extern "C" int run_fmt() {
  int t = 0;
#define C(e, s) do { ++t; if (!eq(e, s)) return t; } while (0)
  C(std::format("REPEATOR {}-tick: {} more", 6, 6 - 4), "REPEATOR 6-tick: 2 more");
  C(std::format("state 0x{:04x} [{}]", 0x2au, "on"), "state 0x002a [on]");
  C(std::format("{:02x}{:02x}", 255, 7), "ff07");
  C(std::format("{:.1f}", 2.25), "2.2");
  C(std::format("{:.1f}", -0.04), "-0.0");
  C(std::format("{}", 0.1), "0.1");
  C(std::format("{}", 1.0 / 3), "0.3333333333333333");
  C(std::format("{}", 2.5f), "2.5");
  C(std::format("{} {}", true, false), "true false");
  C(std::format("{}", std::int64_t(-9223372036854775807LL - 1)), "-9223372036854775808");
  C(std::format("{}", std::uint64_t(18446744073709551615ULL)), "18446744073709551615");
  C(std::format("{{}} {} }}{{", 'x'), "{} x }{");
  C(std::format("[{:5}] [{:5}] [{:05}]", 42, "ab", -3), "[   42] [ab   ] [-0003]");
  C(std::format("{:X} {:b} {:o}", 255, 5, 8), "FF 101 10");
  std::string s = "str"; std::string_view sv = "view";
  C(std::format("{} {} {:.2}", s, sv, s), "str view st");
  std::string o; std::format_to(std::back_inserter(o), "{}-{}", 1, 2);
  C(o, "1-2");
  if (std::formatted_size("{}", 12345) != 5) return 99;
  return 0;
}
