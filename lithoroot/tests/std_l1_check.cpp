// std_l1_check.cpp ... LithoRoot std layer L1 gate (S2, 2026-11-16).
// Compile-time checks (static_assert) plus run_l1() for a runtime host (Node smoke later); returns 0 on pass.
#include <bit>
#include <cassert>
#include <cstdint>
#include <numbers>
#include <tuple>
#include <utility>
struct MoveOnly { int v; MoveOnly(int x) : v(x) {} MoveOnly(MoveOnly&&) = default; MoveOnly& operator=(MoveOnly&&) = default; };
constexpr int sb() { std::pair<int, long> p{1, 2L}; auto [a, b] = p; return a + int(b); }
static_assert(sb() == 3);
constexpr bool pc() { auto p = std::make_pair(1, 2.5); auto q = p; q.first = 0; return q < p && p == std::pair<int, double>(1, 2.5); }
static_assert(pc());
constexpr int tu() {
  std::tuple<int, char, long> t{1, 'a', 3L};
  auto [x, y, z] = t;
  int a = 0; char c = 0;
  std::tie(a, std::ignore, std::ignore) = t;
  (void)c;
  return x + (y == 'a') + int(z) + a + std::get<long>(t) + std::apply([](int p, char, long r) { return int(p + r); }, t);
}
static_assert(tu() == 1 + 1 + 3 + 1 + 3 + 4);
static_assert(std::make_tuple(1, 2) < std::make_tuple(1, 3) && !(std::make_tuple(2, 0) < std::make_tuple(1, 9)) && std::make_tuple(1, 2) == std::make_tuple(1, 2));
static_assert(std::tuple_size_v<std::tuple<int, int, int>> == 3 && std::is_same_v<std::tuple_element_t<1, std::tuple<int, char>>, char>);
static_assert(std::is_same_v<std::make_index_sequence<3>, std::index_sequence<0, 1, 2>>);
static_assert(std::cmp_less(-1, 0u) && !std::cmp_equal(-1, 0xFFFFFFFFu) && std::cmp_greater(1u, -5));
static_assert(std::countr_zero(8u) == 3 && std::countl_zero(uint8_t(1)) == 7 && std::countl_zero(uint64_t(1)) == 63 && std::countr_zero(uint64_t(0)) == 64);
static_assert(std::popcount(0xF0F0u) == 8 && std::bit_width(5u) == 3 && std::bit_ceil(5u) == 8u && std::bit_floor(5u) == 4u && std::has_single_bit(64u));
static_assert(std::rotl(uint8_t(0x81), 1) == 0x03 && std::rotr(uint32_t(1), 1) == 0x80000000u && std::byteswap(uint32_t(0x11223344)) == 0x44332211u);
static_assert(std::bit_cast<uint32_t>(1.0f) == 0x3f800000u && std::endian::native == std::endian::little);
static_assert(std::numbers::pi > 3.14159 && std::numbers::pi_v<float> < 3.1416f);
static_assert(std::is_swappable_v<int> && std::is_nothrow_swappable_v<MoveOnly>);
extern "C" int run_l1() {
  MoveOnly a(1), b(2);
  std::swap(a, b);
  std::tuple<MoveOnly, int> t(MoveOnly(7), 8);
  auto t2 = std::move(t);
  int arr1[2] = {1, 2}, arr2[2] = {3, 4};
  std::swap(arr1, arr2);
  assert(a.v == 2 && b.v == 1);
  return (a.v == 2 && b.v == 1 && std::get<0>(t2).v == 7 && arr1[0] == 3 && std::exchange(arr2[1], 9) == 2 && arr2[1] == 9) ? 0 : 1;
}
