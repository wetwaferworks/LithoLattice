// Cortana 2026-10-16 [ENGINE] (JD; Herominde anti-cheat review): vehicle interface STUBS.
// IVehicle / IGlider are small opt-in contracts in the IBlock / ITickable style: a vehicle type
// implements only what it needs, one shared behavior object per type (flyweight), per-instance
// state lives in VehicleState. No gameplay content ships with these (no vehicle exists yet:
// Tron light-cycle bikes and gliders are the planned first users).
//
// AUTHORITY (Herominde): the authoritative tick owns the movement.
//  - The ONLY input a vehicle receives is VehicleControls: the controls pressed this tick (bits +
//    clamped analog axes). Never a position, velocity or orientation from the client.
//  - onTick computes speed / thrust / lift / fuel deterministically from (state, controls, tick
//    context) and writes VehicleState. The engine integrates position from that state; there is
//    no API that lets a vehicle or a mod set a position from client input.
//  - Everything is plain integer / fixed-point-friendly data and tick-driven, so it can be
//    stamped into the input stream and replayed bit-exactly.
// THREADING NOTE: player movement currently runs on the GAME thread (locomotion body, per sim
// tick, not on the sim thread). These interfaces carry no game-thread assumptions (no
// renderer, input-device or wall-clock access), so they are ready to move into the sim with
// player movement when that refactor lands. Nothing calls them yet: the handshake world hash
// is unaffected.
#pragma once
#include <cstdint>

namespace litholattice {

/// Controls pressed this tick, as stamped into the input stream. Bits + clamped axes only.
struct VehicleControls {
  enum Bit : uint32_t {
    kThrottle = 1u << 0, kBrake = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3,
    kPitchUp = 1u << 4, kPitchDown = 1u << 5, kBoost = 1u << 6, kAction = 1u << 7,
  };
  uint32_t bits = 0;
  int8_t steer = 0;     ///< -127..127 analog steer (clamped by the engine)
  int8_t throttle = 0;  ///< -127..127 analog throttle (clamped by the engine)
  bool has(Bit b) const { return (bits & b) != 0; }
  static constexpr uint32_t kKnownBits = 0xFFu;
  /// 2026-10-16 review: the clamp the comments promise. Unknown bits are dropped and the
  /// axes are held to -127..127 (int8 -128 has no mirror and would bias a symmetric steer).
  /// The engine applies this to every stamped control before onTick.
  VehicleControls clamped() const {
    VehicleControls c = *this;
    c.bits &= kKnownBits;
    if (c.steer < -127) c.steer = -127;
    if (c.throttle < -127) c.throttle = -127;
    return c;
  }
};

/// Per-vehicle simulated state. Written ONLY by the authoritative tick (onTick).
struct VehicleState {
  int32_t speed = 0;    ///< blocks/s in 1/256 units (fixed point)
  int32_t thrust = 0;   ///< 1/256 units, this tick's computed thrust
  int32_t lift = 0;     ///< 1/256 units (gliders; 0 for ground vehicles)
  int32_t fuel = 0;     ///< game-defined units
  uint64_t riderId = 0; ///< 0 = unmounted
};

/// Per-tick context for a vehicle (mirrors TickContext: tick index, rate, deterministic rng).
struct VehicleTickContext {
  uint64_t tick = 0;
  int tps = 32;
  uint64_t rng = 0;  ///< deterministic per vehicle + tick
};

/// A rideable vehicle. Opt-in: a type implements IVehicle only if it can be ridden.
class IVehicle {
 public:
  virtual ~IVehicle() = default;
  /// A rider asks to mount. Return false to refuse (occupied, wrong side, ...).
  virtual bool onMount(VehicleState& state, uint64_t riderId) = 0;
  /// The rider leaves (dismount control, death, unload).
  virtual void onDismount(VehicleState& state) = 0;
  /// One authoritative tick: controls in, state out. Must be deterministic in its arguments.
  virtual void onTick(const VehicleTickContext& ctx, const VehicleControls& controls, VehicleState& state) = 0;
  /// Speed / thrust hook: thrust (1/256 units) for these controls at this state. Pure.
  virtual int32_t thrustFor(const VehicleControls& controls, const VehicleState& state) const = 0;
};

/// 2026-10-16 review: the engine-side mount gate. Refuses rider 0 and an occupied seat before
/// the type's own onMount runs, and records the rider only when onMount accepts. A vehicle type
/// cannot accept a second rider by forgetting the check.
inline bool tryMount(IVehicle& v, VehicleState& state, uint64_t riderId) {
  if (riderId == 0 || state.riderId != 0) return false;
  if (!v.onMount(state, riderId)) return false;
  state.riderId = riderId;
  return true;
}
/// The matching dismount: no-op when nobody rides; clears the rider after the hook.
inline void dismount(IVehicle& v, VehicleState& state) {
  if (state.riderId == 0) return;
  v.onDismount(state);
  state.riderId = 0;
}

/// A gliding vehicle. Opt-in on top of IVehicle.
class IGlider : public IVehicle {
 public:
  /// Lift hook (1/256 units) at the current speed and pitch controls. Pure.
  virtual int32_t liftFor(const VehicleControls& controls, const VehicleState& state) const = 0;
  /// Glide ratio (horizontal : vertical) in 1/256 units, e.g. 10:1 = 2560. Pure.
  virtual int32_t glideRatio(const VehicleState& state) const = 0;
};

}  // namespace litholattice
