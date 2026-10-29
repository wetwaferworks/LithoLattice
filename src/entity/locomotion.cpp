#include "litholattice/entity/locomotion.h"

#include <algorithm>
#include <cmath>

#include "litholattice/entity/physics.h"

namespace litholattice {
namespace {

double heightFor(const LocomotionConfig& c, LocomotionPose p) {
  switch (p) {
    case LocomotionPose::Stand: return c.heightStand;
    case LocomotionPose::Sneak: return c.heightSneak;
    case LocomotionPose::Crawl: return c.heightCrawl;
    case LocomotionPose::Kneel: return c.heightKneel;
  }
  return c.heightStand;
}

double eyeFor(const LocomotionConfig& c, LocomotionPose p) {
  switch (p) {
    case LocomotionPose::Stand: return c.eyeStand;
    case LocomotionPose::Sneak: return c.eyeSneak;
    case LocomotionPose::Crawl: return c.eyeCrawl;
    case LocomotionPose::Kneel: return c.eyeKneel;
  }
  return c.eyeStand;
}

/// Next pose down (Stand -> Sneak -> Crawl); Crawl maps to itself.
LocomotionPose shorter(LocomotionPose p) {
  switch (p) {
    case LocomotionPose::Stand: return LocomotionPose::Sneak;
    case LocomotionPose::Sneak: return LocomotionPose::Kneel;
    case LocomotionPose::Kneel: return LocomotionPose::Crawl;
    case LocomotionPose::Crawl: return LocomotionPose::Crawl;
  }
  return LocomotionPose::Crawl;
}

/// True when the AABB [x-hw, x+hw] x [y, y+h] x [z-hw, z+hw] overlaps no
/// solid voxel. Deliberately the same floor-based scan physics.cpp's
/// aabbCollides uses, so "fits" here and "collides" there never disagree.
bool fitsAt(double x, double y, double z, double hw, double h,
            const std::function<bool(int, int, int)>& isSolid) {
  const int x0 = int(std::floor(x - hw)), x1 = int(std::floor(x + hw));
  const int y0 = int(std::floor(y)), y1 = int(std::floor(y + h));
  const int z0 = int(std::floor(z - hw)), z1 = int(std::floor(z + hw));
  for (int vx = x0; vx <= x1; ++vx)
    for (int vy = y0; vy <= y1; ++vy)
      for (int vz = z0; vz <= z1; ++vz)
        if (isSolid(vx, vy, vz)) return false;
  return true;
}

/// True when any solid voxel lies under the footprint (x/z AABB columns) in
/// the slab from the feet down to `depth` blocks below — the sneak
/// edge-guard's support test.
bool supportBelow(double x, double y, double z, double hw, double depth,
                  const std::function<bool(int, int, int)>& isSolid) {
  const int x0 = int(std::floor(x - hw)), x1 = int(std::floor(x + hw));
  const int z0 = int(std::floor(z - hw)), z1 = int(std::floor(z + hw));
  const int yTop = int(std::floor(y - 1e-9));   // first voxel below the feet
  const int yBot = int(std::floor(y - depth)); // deepest voxel still "within depth"
  for (int vx = x0; vx <= x1; ++vx)
    for (int vy = yBot; vy <= yTop; ++vy)
      for (int vz = z0; vz <= z1; ++vz)
        if (isSolid(vx, vy, vz)) return true;
  return false;
}

/// Derived jump velocity: v = sqrt(2 * g * h), so the ballistic apex under
/// the config's gravity is exactly jumpHeight (up to integration error).
double jumpVelocity(const LocomotionConfig& c) {
  return std::sqrt(2.0 * c.gravity * c.jumpHeight);
}

}  // namespace

double cLocomotionController::eyeHeight() const { return eyeFor(cfg_, pose_); }
double cLocomotionController::currentHeight() const { return heightFor(cfg_, pose_); }

void cLocomotionController::step(Entity& e, const LocomotionInput& in,
                                const std::function<bool(int, int, int)>& isSolid,
                                double dt) {
  if (!(dt > 0.0)) return;  // no time passes, no motion (also rejects NaN)

  // -- 1. Pose resolution (mode-independent): the tallest pose at or below
  // the desired one whose AABB fits here. Growing requires room; a space
  // that only fits Crawl forces Crawl; if nothing fits, Crawl is kept.
  // Cortana 2026-10-15 2240 [ENGINE] (JD 22:36): kneel from the sneak bit's history. Each
  // false->true edge is a tap stamped with step time; 3 taps within kneelTapWindow latch kneel.
  // While kneeling, the next tap, a jump, Fly mode or crawl releases it. Pure function of the
  // input sequence and dt, so it is replay-deterministic.
  clock_ += dt;
  const bool sneakEdge = in.sneak && !prevSneak_;
  prevSneak_ = in.sneak;
  if (mode_ != LocomotionMode::Walk || in.jump || in.crawl) {
    kneel_ = false;
  } else if (sneakEdge) {
    if (kneel_) {
      kneel_ = false;
      tapT_[0] = tapT_[1] = tapT_[2] = -1e9;
    } else {
      tapT_[0] = tapT_[1]; tapT_[1] = tapT_[2]; tapT_[2] = clock_;
      if (tapT_[2] - tapT_[0] <= cfg_.kneelTapWindow) {
        kneel_ = true;
        tapT_[0] = tapT_[1] = tapT_[2] = -1e9;
      }
    }
  }
  const LocomotionPose desired = in.crawl  ? LocomotionPose::Crawl
                                 : kneel_  ? LocomotionPose::Kneel
                                 : in.sneak ? LocomotionPose::Sneak
                                            : LocomotionPose::Stand;
  pose_ = desired;
  while (pose_ != LocomotionPose::Crawl &&
         !fitsAt(e.x, e.y, e.z, cfg_.halfWidth, heightFor(cfg_, pose_), isSolid))
    pose_ = shorter(pose_);

  // Wish direction, clamped to magnitude 1 (partial = partial speed).
  double wishX = in.wishX, wishZ = in.wishZ;
  const double wishLen = std::hypot(wishX, wishZ);
  if (wishLen > 1.0) {
    wishX /= wishLen;
    wishZ /= wishLen;
  }

  // -- 1b. Heading probe: if the resolved pose would not fit where this
  // step's intended horizontal travel lands, but a shorter pose would fit
  // there, lower now. Without this, a walking body stops flush against a
  // 1-block tunnel mouth forever: the sweep clamps the AABB 1e-4 short of
  // the boundary voxel, so a purely positional fit test can never see the
  // tunnel. If NO pose fits at the destination (a wall, a step), nothing
  // changes — the sweep clamps as usual.
  if (pose_ != LocomotionPose::Crawl && (wishX != 0.0 || wishZ != 0.0)) {
    double probeSpeed = in.sprint ? cfg_.flySpeed * cfg_.flySprintMultiplier : cfg_.flySpeed;
    if (mode_ == LocomotionMode::Walk) {
      probeSpeed = (desired == LocomotionPose::Sneak || desired == LocomotionPose::Kneel) ? cfg_.sneakSpeed
                   : desired == LocomotionPose::Crawl ? cfg_.crawlSpeed
                   : (in.sprint ? cfg_.walkSpeed * cfg_.sprintMultiplier : cfg_.walkSpeed);
    }
    const double px = e.x + wishX * probeSpeed * dt;
    const double pz = e.z + wishZ * probeSpeed * dt;
    if (!fitsAt(px, e.y, pz, cfg_.halfWidth, heightFor(cfg_, pose_), isSolid)) {
      for (LocomotionPose p = shorter(pose_);; p = shorter(p)) {
        if (fitsAt(px, e.y, pz, cfg_.halfWidth, heightFor(cfg_, p), isSolid)) {
          pose_ = p;
          break;
        }
        if (p == LocomotionPose::Crawl) break;  // wall: keep pose_, sweep clamps
      }
    }
  }
  const double height = heightFor(cfg_, pose_);
  const float hw = float(cfg_.halfWidth);

  // -- Targets for this step.
  double targetVx = 0, targetVz = 0, rate = cfg_.groundAccel;
  if (mode_ == LocomotionMode::Fly) {
    // 2026-10-24 (JD): Sprint while flying = faster flight, like Minecraft.
    const double fs = in.sprint ? cfg_.flySpeed * cfg_.flySprintMultiplier : cfg_.flySpeed;
    targetVx = wishX * fs;
    targetVz = wishZ * fs;
  } else {
    double speed = cfg_.walkSpeed;
    if (pose_ == LocomotionPose::Sneak || pose_ == LocomotionPose::Kneel) speed = cfg_.sneakSpeed;  // Cortana 2026-10-15 2240
    if (pose_ == LocomotionPose::Crawl) speed = cfg_.crawlSpeed;
    if (pose_ == LocomotionPose::Stand && in.sprint) speed = cfg_.walkSpeed * cfg_.sprintMultiplier;
    targetVx = wishX * speed;
    targetVz = wishZ * speed;
    rate = e.onGround ? cfg_.groundAccel : cfg_.airAccel;
  }

  // -- Sneak edge-guard bookkeeping (Walk + Sneak + grounded, pre-move).
  const bool guardArmed = mode_ == LocomotionMode::Walk && pose_ == LocomotionPose::Sneak && e.onGround;
  const bool groundedAtStart = e.onGround;  // the auto-step's licence
  const double preX = e.x, preY = e.y, preZ = e.z;

  // -- Substep count: moveCollide moves discretely per axis, so cap each
  // substep's per-axis travel at ~0.5 blocks — a step of < 1 block can
  // never skip a 1-block-thick obstacle (the destination AABB is >= 0.6
  // tall/wide), and 0.5 leaves margin. The worst speed the step can reach
  // bounds the count: current velocity, the gravity clamp, the targets.
  double worst = std::max({std::fabs(e.vx), std::fabs(e.vy), std::fabs(e.vz),
                           std::fabs(targetVx), std::fabs(targetVz)});
  if (mode_ == LocomotionMode::Walk) worst = std::max(worst, std::fabs(cfg_.terminalVelocity));
  if (mode_ == LocomotionMode::Fly) worst = std::max(worst, std::fabs(cfg_.flyVerticalSpeed));
  int substeps = int(std::ceil(worst * dt / 0.5));
  substeps = std::clamp(substeps, 1, 64);
  const double h = dt / substeps;

  const double approach = 1.0 - std::exp(-rate * h);  // per-substep factor
  const bool jumpHeld = in.jump;                      // consumed on substep 0 only

  for (int i = 0; i < substeps; ++i) {
    e.vx += (targetVx - e.vx) * approach;
    e.vz += (targetVz - e.vz) * approach;
    if (mode_ == LocomotionMode::Fly) {
      e.vy = (in.jump ? cfg_.flyVerticalSpeed : 0.0) +
             (in.sneak ? -cfg_.flyVerticalSpeed : 0.0);
    } else {
      if (i == 0 && jumpHeld && e.onGround) e.vy = jumpVelocity(cfg_);
      e.vy -= cfg_.gravity * h;
      if (e.vy < cfg_.terminalVelocity) e.vy = cfg_.terminalVelocity;
    }
    moveCollide(e, isSolid, hw, float(height), h);
  }

  // -- Auto-step (Litho Strike P3, 2026-10-26; config stepHeight, default 0 =
  // off): a grounded, standing Walk body that was blocked horizontally (it
  // got less than half its intended horizontal travel) retries the move one
  // step higher. Review M2/M3 (2026-10-29): the retry is a real swept move,
  // not a teleport. (1) The lift needs headroom where the body stands. (2)
  // The horizontal move is swept at the raised height through moveCollide,
  // the same per-axis clamp as walking, so an obstacle anywhere along the
  // raised path stops it (no corner clipping at large dt). (3) One swept
  // downward move settles onto the step's top, replacing the old 1/32-block
  // probe loop. The lift is clamped to kMaxStepHeight. On acceptance the
  // horizontal velocity is restored to the step's targets: stepping must not
  // kill momentum. Pure function of (state, input, world, dt).
  const double stepLift = std::min(cfg_.stepHeight, kMaxStepHeight);
  if (mode_ == LocomotionMode::Walk && stepLift > 0.0 && groundedAtStart && e.onGround &&
      pose_ == LocomotionPose::Stand && !in.jump && (wishX != 0.0 || wishZ != 0.0)) {
    const double ix = targetVx * dt, iz = targetVz * dt;
    const double dx = e.x - preX, dz = e.z - preZ;
    const double intended2 = ix * ix + iz * iz;
    const double moved2 = dx * dx + dz * dz;
    if (intended2 > 0.0 && moved2 < 0.25 * intended2 &&
        fitsAt(preX, preY + stepLift, preZ, cfg_.halfWidth, height, isSolid)) {
      Entity t = e;
      t.x = preX;
      t.y = preY + stepLift;
      t.z = preZ;
      t.vx = targetVx;
      t.vy = 0.0;
      t.vz = targetVz;
      moveCollide(t, isSolid, hw, float(height), dt);
      const double rx = t.x - preX, rz = t.z - preZ;
      if (rx * rx + rz * rz > moved2) {  // the raised path really got further
        t.vx = 0.0;
        t.vz = 0.0;
        t.vy = -(stepLift + 0.5);  // one swept drop of lift + 0.5 over unit time
        moveCollide(t, isSolid, hw, float(height), 1.0);
        if (t.onGround && t.y > preY + 0.5 * stepLift) {
          e.x = t.x;
          e.y = t.y;
          e.z = t.z;
          e.vx = targetVx;
          e.vz = targetVz;
          e.vy = 0.0;
          e.onGround = true;
        }
      }
    }
  }
  // -- Sneak edge-guard verdict: a grounded sneak that ended airborne with
  // no support within 1.05 blocks below the new footprint never meant to
  // leave — restore the full pre-move position and the grounded flag (the
  // pre-move support state), as if the step had not moved. Restoring y as
  // well as x/z is load-bearing, not cosmetic: a kept mid-step y sits a
  // few millimetres BELOW the support's top surface, where the fit test's
  // voxel scan includes the supporting row itself and reports every pose
  // as unfit — the pose collapses to Crawl, which disarms this guard
  // (Sneak-only) on the next step and the body walks off after all.
  if (guardArmed && !e.onGround &&
      !supportBelow(e.x, e.y, e.z, cfg_.halfWidth, 1.05, isSolid)) {
    e.x = preX;
    e.y = preY;
    e.z = preZ;
    e.onGround = true;
  }

  if (mode_ == LocomotionMode::Walk && e.onGround && e.vy < 0.0) e.vy = 0.0;
}

}  // namespace litholattice
