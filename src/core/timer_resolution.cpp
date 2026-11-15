#include "litholattice/core/timer_resolution.h"
#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod: 1 ms scheduler granularity
#endif

namespace litholattice {

#if defined(_WIN32)
HighResTimerScope::HighResTimerScope() : active_(timeBeginPeriod(1) == TIMERR_NOERROR) {}
HighResTimerScope::~HighResTimerScope() {
  if (active_) timeEndPeriod(1);
}
// 0.0.1.57 F57-1 (R21)
void optOutOfBackgroundThrottling() {
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  s.StateMask = 0;  // control the bit and leave it clear: never throttle execution speed
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s);
}
void raiseCurrentThreadForSim() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#if defined(__MINGW32__) || defined(__MINGW64__)
  // Elfie winbuild patch (bench copy only, NOT pushed): Ubuntu mingw-w64 headers do not carry
  // THREAD_POWER_THROTTLING_STATE (winnt.h). Local definition matching the Windows SDK layout;
  // ThreadPowerThrottling (=7) is present in mingw's THREAD_INFORMATION_CLASS (winbase.h).
  struct LkThreadPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
  };
  LkThreadPowerThrottlingState t{};
  t.Version = 1;             // THREAD_POWER_THROTTLING_CURRENT_VERSION
  t.ControlMask = 0x1;       // THREAD_POWER_THROTTLING_EXECUTION_SPEED
  t.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &t, sizeof t);
#else
  THREAD_POWER_THROTTLING_STATE t{};
  t.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  t.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  t.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &t, sizeof t);
#endif
}
#else
HighResTimerScope::HighResTimerScope() = default;
HighResTimerScope::~HighResTimerScope() = default;
void optOutOfBackgroundThrottling() {}
void raiseCurrentThreadForSim() {}
#endif

}  // namespace litholattice
