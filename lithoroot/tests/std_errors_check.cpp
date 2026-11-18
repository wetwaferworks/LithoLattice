// std_errors_check.cpp ... LithoRoot std L7 <system_error> + L6 <bitset> gate (S4, 2026-11-16).
// Compile-and-link check plus value checks that need no strerror (Node host).
#include <bitset>
#include <system_error>
extern "C" int run_err() {
  std::error_code e = std::make_error_code(std::errc::no_such_file_or_directory);
  if (!e || e != std::errc::no_such_file_or_directory || e == std::errc::file_exists) return 1;
  if (&e.category() != &std::generic_category() || std::string(e.category().name()) != "generic") return 2;
  std::error_code ok;
  if (ok || ok.category() != std::system_category()) return 3;
  std::bitset<70> b;
  b.set(0); b.set(69); b[33] = true;
  if (b.count() != 3 || !b.test(69) || (b << 1).test(69) || !(b >> 69).test(0)) return 4;
  if (std::bitset<8>(0xA5).to_string() != "10100101" || std::bitset<8>(std::string("1100")).to_ullong() != 12) return 5;
  if ((~std::bitset<5>()).count() != 5) return 6;
  return 0;
}
