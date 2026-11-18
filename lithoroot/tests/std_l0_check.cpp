// std_l0_check.cpp ... LithoRoot std layer L0 compile-time gate (S2, 2026-11-16).
// Build: clang++ --target=wasm32-unknown-unknown --sysroot=engine/lithoroot -std=c++20 -Wall -Wextra -c
// Passing = it compiles (every check is a static_assert); g()/h()/nanf_() exercise codegen.
#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <initializer_list>
#include <new>
#include <cstddef>
#include <climits>
#include <cfloat>
#include <version>
struct A { virtual ~A(); }; struct B : A {}; enum class E : short {};
struct M { int f(int); int x; };
static_assert(std::is_same_v<std::remove_cvref_t<const int&>, int>);
static_assert(std::is_base_of_v<A, B> && !std::is_base_of_v<B, A>);
static_assert(std::is_same_v<std::underlying_type_t<E>, short>);
static_assert(std::is_same_v<std::decay_t<int[3]>, int*>);
static_assert(std::is_same_v<std::make_unsigned_t<long long>, unsigned long long>);
static_assert(std::is_same_v<std::common_type_t<int, long, short>, long>);
static_assert(std::is_same_v<std::invoke_result_t<int (M::*)(int), M&, int>, int>);
static_assert(std::is_same_v<std::invoke_result_t<int M::*, const M&>, const int&>);
static_assert(std::is_invocable_r_v<long, int(*)(int), short> && !std::is_invocable_v<int(*)(int), M>);
static_assert(std::is_trivially_copyable_v<M> && !std::is_trivially_copyable_v<A>);
static_assert(std::is_nothrow_move_constructible_v<M> && std::has_virtual_destructor_v<B>);
static_assert(std::conjunction_v<std::true_type, std::is_integral<int>> && !std::disjunction_v<std::false_type>);
static_assert(std::extent_v<int[4][5], 1> == 5 && std::rank_v<int[4][5]> == 2);
static_assert(std::is_scoped_enum_v<E> && std::is_signed_v<int> && std::is_unsigned_v<unsigned>);
int sum(std::initializer_list<int> l) { int s = 0; for (int v : l) s += v; return s; }
int g() { return sum({1, 2, 3}); }
void* h(void* p) { return new (p) M{}; }
static_assert(std::to_integer<int>(std::byte{3} | std::byte{4}) == 7);
static_assert(INT_MAX == 2147483647 && FLT_MANT_DIG == 24);
struct P { int a, b; auto operator<=>(const P&) const = default; bool operator==(const P&) const = default; };
static_assert((P{1,2} <=> P{1,3}) < 0 && P{2,0} > P{1,9} && P{1,1} == P{1,1});
static_assert(std::is_lt(1 <=> 2) && std::is_eq(3 <=> 3) && std::is_gt(2.0 <=> 1.0));
static_assert((1.0 <=> __builtin_nan("")) == std::partial_ordering::unordered);
static_assert(std::is_same_v<decltype(1 <=> 2), std::strong_ordering>);
static_assert((0 <=> (1 <=> 2)) > 0);
static_assert(std::numeric_limits<int>::max() == 2147483647 && std::numeric_limits<int>::min() == -2147483647 - 1);
static_assert(std::numeric_limits<uint64_t>::max() == 18446744073709551615ull && std::numeric_limits<uint8_t>::digits == 8);
static_assert(std::numeric_limits<int64_t>::digits == 63 && std::numeric_limits<int>::digits10 == 9 && std::numeric_limits<uint64_t>::digits10 == 19);
static_assert(std::numeric_limits<float>::digits == 24 && std::numeric_limits<double>::max_digits10 == 17 && std::numeric_limits<float>::max_digits10 == 9);
static_assert(std::numeric_limits<float>::infinity() > std::numeric_limits<float>::max());
static_assert(std::numeric_limits<double>::lowest() == -std::numeric_limits<double>::max());
static_assert(std::numeric_limits<bool>::digits == 1 && std::numeric_limits<char>::is_signed && std::numeric_limits<long>::max() == 2147483647L);
static_assert(std::numeric_limits<const unsigned>::max() == 4294967295u);
static_assert(std::numeric_limits<float>::epsilon() == 0x1p-23f);
float nanf_() { return std::numeric_limits<float>::quiet_NaN(); }
