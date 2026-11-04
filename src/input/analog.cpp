#include "litholattice/input/analog.h"
#include <algorithm>
#include <cmath>

namespace litholattice {

AxisValue applyRadialDeadzone(AxisValue raw, float deadzone, float curve) {
  const float mag = std::hypot(raw.x, raw.y);
  if (mag <= deadzone || mag <= 0.f || deadzone >= 1.f) return {};  // Archie review H29f: /0
  const float scaled = std::pow(std::min(1.f, (mag - deadzone) / (1.f - deadzone)), curve);
  return {raw.x / mag * scaled, raw.y / mag * scaled};
}

float applyDeadzone(float raw, float deadzone, float curve) {
  const float mag = std::abs(raw);
  if (mag <= deadzone || deadzone >= 1.f) return 0.f;  // Archie review H29f: /0
  return std::copysign(std::pow(std::min(1.f, (mag - deadzone) / (1.f - deadzone)), curve), raw);
}

}  // namespace litholattice
