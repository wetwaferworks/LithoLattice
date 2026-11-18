// std_l5_check.cpp ... LithoRoot std L5 gate, part 1 (S3, 2026-11-16): string_view,
// char_traits, charconv (ints), stdexcept. run_l5() returns 0 on pass (Node host).
#include <charconv>
#include <stdexcept>
#include <string_view>

static_assert(std::string_view("abc").size() == 3);
static_assert(std::string_view("hello world").find("wor") == 6);
static_assert(std::string_view("a,b,c").rfind(',') == 3);
static_assert(std::string_view("abc") < std::string_view("abd"));
static_assert(std::string_view("abc").starts_with("ab") && std::string_view("abc").ends_with('c'));

namespace {
std::from_chars_result fc(std::string_view s, auto& v, int base = 10) { return std::from_chars(s.data(), s.data() + s.size(), v, base); }
}  // namespace

extern "C" int run_l5() {
  int c = 0;
#define CHECK(x) do { ++c; if (!(x)) return c; } while (0)
  using namespace std::literals;
  std::string_view s = "  key = value  "sv;
  auto b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
  CHECK(b == 2 && e == 12 && s.substr(b, e - b + 1) == "key = value");
  CHECK(s.find_first_of("=") == 6 && s.find("zz") == std::string_view::npos && s.find("") == 0);
  CHECK(s.rfind("") == s.size() && "abc"sv.rfind("abc") == 0 && "aaa"sv.rfind("aa") == 1);
  CHECK("abc"sv.compare("abcd") < 0 && "b"sv.compare("abcd") > 0 && "abc"sv == "abc");
  CHECK(("\xff"sv > "\x01"sv));  // unsigned char ordering, as the standard requires
  CHECK(std::hash<std::string_view>()("") == 2166136261u && std::hash<std::string_view>()("a") == 0xe40c292cu);  // FNV-1a test vectors
  char buf[64];
  auto r = std::to_chars(buf, buf + 64, -2147483647 - 1);
  CHECK(r.ec == std::errc{} && std::string_view(buf, size_t(r.ptr - buf)) == "-2147483648");
  r = std::to_chars(buf, buf + 64, 18446744073709551615ull);
  CHECK(std::string_view(buf, size_t(r.ptr - buf)) == "18446744073709551615");
  r = std::to_chars(buf, buf + 64, 255, 16);
  CHECK(std::string_view(buf, size_t(r.ptr - buf)) == "ff");
  r = std::to_chars(buf, buf + 2, 12345);
  CHECK(r.ec == std::errc::value_too_large);
  int v = 0;
  auto f = fc("-123xyz", v);
  CHECK(f.ec == std::errc{} && v == -123 && *f.ptr == 'x');
  f = fc("2147483648", v);
  CHECK(f.ec == std::errc::result_out_of_range && v == -123);
  f = fc("-2147483648", v);
  CHECK(f.ec == std::errc{} && v == -2147483647 - 1);
  unsigned u = 7;
  f = fc("-1", u);
  CHECK(f.ec == std::errc::invalid_argument && u == 7);
  f = fc("zz", u, 36);
  CHECK(u == 35u * 36u + 35u);
  return 0;
#undef CHECK
}
