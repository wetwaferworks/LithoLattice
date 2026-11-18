// std_l2_check.cpp ... LithoRoot std layer L2 gate (S2, 2026-11-16): the C wrappers.
// run_l2() returns 0 on pass (Node host). The overload checks are the point: float stays float.
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <type_traits>
static_assert(std::is_same_v<decltype(std::sqrt(1.0f)), float> && std::is_same_v<decltype(std::floor(2.5f)), float>);
static_assert(std::is_same_v<decltype(std::sqrt(2)), double> && std::is_same_v<decltype(std::abs(-1.5f)), float>);
static_assert(std::is_same_v<decltype(std::pow(2.0f, 3.0f)), float> && std::is_same_v<decltype(std::pow(2.0f, 3)), double>);
static_assert(std::is_same_v<decltype(std::abs(-3L)), long> && std::is_same_v<decltype(std::atan2(1.0f, 2.0f)), float>);
static_assert(std::is_same_v<int64_t, long long> && std::is_same_v<intptr_t, long> && sizeof(void*) == 4);
static_assert(std::lerp(0.0f, 10.0f, 0.5f) == 5.0f && std::lerp(1.0, 3.0, 1.0) == 3.0);
extern "C" int run_l2() {
  int bad = 0;
  volatile float f = 2.25f;
  if (std::sqrt(f) != 1.5f) bad |= 1;
  if (!std::isnan(std::sqrt(-f)) || std::isinf(1.0) || !std::isfinite(f) || std::signbit(f)) bad |= 2;
  if (std::rint(2.5f) != 2.0f || std::rint(3.5) != 4.0 || std::nearbyint(-0.5) != 0.0) bad |= 4;
  if (std::abs(-7) != 7 || std::abs(-7LL) != 7LL || std::fabs(-f) != f) bad |= 8;
  if (std::fpclassify(0.0f) != FP_ZERO || std::fpclassify(f) != FP_NORMAL) bad |= 16;
  if (!std::isdigit('7') || std::toupper('a') != 'A') bad |= 32;
  return bad;
}
