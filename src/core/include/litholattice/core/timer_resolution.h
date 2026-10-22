// Scoped request for a fine OS scheduler tick, so short sleeps are accurate (Windows defaults to
// ~15.6 ms; this asks for 1 ms while the scope lives). No-op on platforms that do not need it.
#pragma once

namespace litholattice {

// 0.0.1.57 F57-1 (R21; Discovered by Cortana, Fixed by Cortana): keep the simulation at full speed while another
// program (e.g. a second LithoKraft) loads. optOutOfBackgroundThrottling() asks Windows not to EcoQoS/power-throttle
// this process when its window is in the background; raiseCurrentThreadForSim() puts the calling thread one step
// above normal priority and opts it out of power throttling. Both are best-effort no-ops elsewhere.
void optOutOfBackgroundThrottling();
void raiseCurrentThreadForSim();

class HighResTimerScope {
 public:
  HighResTimerScope();
  ~HighResTimerScope();
  HighResTimerScope(const HighResTimerScope&) = delete;
  HighResTimerScope& operator=(const HighResTimerScope&) = delete;

 private:
  bool active_ = false;
};

}  // namespace litholattice
