// std_chrono_check.cpp ... LithoRoot std L8 <chrono>/<ratio> gate (S4, 2026-11-16): compile-time
// duration arithmetic (static_asserts) plus a run that reads the clock through a host stub.
#include <chrono>
using namespace std::chrono;
using namespace std::chrono_literals;
static_assert(duration_cast<milliseconds>(1500us).count() == 1);
static_assert(duration_cast<microseconds>(2s).count() == 2000000);
static_assert((1s + 500ms).count() == 1500);
static_assert(std::is_same_v<decltype(1s + 500ms), milliseconds>);
static_assert(floor<seconds>(milliseconds(-1500)).count() == -2 && ceil<seconds>(1200ms).count() == 2);
static_assert(round<seconds>(1500ms).count() == 2 && round<seconds>(2500ms).count() == 2);
static_assert(1min == 60s && 1h > 59min && (90s / 30s) == 3);
static_assert(duration<double>(250ms).count() == 0.25);
static_assert(std::ratio_multiply<std::milli, std::kilo>::num == 1 && std::ratio_add<std::ratio<1, 3>, std::ratio<1, 6>>::den == 2);
extern "C" int run_chrono() {
  auto a = steady_clock::now();
  auto b = steady_clock::now();
  if (b < a) return 1;
  auto d = duration_cast<microseconds>(b - a);
  if (d.count() < 0) return 2;
  if (system_clock::to_time_t(system_clock::from_time_t(1234567)) != 1234567) return 3;
  return 0;
}
