#include "litholattice/world/raycast.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace litholattice {

void flatPickRange(const ChunkMap& w, const cBlockReg& blocks, int x, int y, int z, float& y0, float& y1) {
  const bool hanging = !blocks.has(w.get(x, y - 1, z), kSolid) && blocks.has(w.get(x, y + 1, z), kSolid);
  y0 = float(y) + (hanging ? 15.f / 16.f : 0.f);
  y1 = y0 + 1.f / 16.f;
}

static bool rayHitsBox(const float o[3], const float d[3], const float lo[3], const float hi[3], float maxDist) {
  float t0 = 0.f, t1 = maxDist;
  for (int a = 0; a < 3; ++a) {
    if (std::abs(d[a]) < 1e-9f) { if (o[a] < lo[a] || o[a] > hi[a]) return false; continue; }
    float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta); t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

static std::atomic<PickShapeFn> gPickShape{nullptr};
void setPickShapeFn(PickShapeFn fn) { gPickShape.store(fn, std::memory_order_release); }

bool raycast(const ChunkMap& w, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, int hit[3], int prev[3], const cBlockReg* blocks, bool modelPick) {
  const PickShapeFn pickShape = (modelPick && blocks) ? gPickShape.load(std::memory_order_acquire) : nullptr;
  int c[3] = {int(std::floor(ox)), int(std::floor(oy)), int(std::floor(oz))};
  const float o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz};
  int step[3]; float tMax[3], tDelta[3];
  for (int a = 0; a < 3; ++a) {
    step[a] = d[a] > 0 ? 1 : -1;
    tDelta[a] = d[a] != 0 ? std::abs(1.f / d[a]) : 1e30f;
    const float edge = d[a] > 0 ? float(c[a] + 1) - o[a] : o[a] - float(c[a]);
    tMax[a] = d[a] != 0 ? edge * tDelta[a] : 1e30f;
  }
  // Test the origin cell first: the DDA below steps before testing, so a ray
  // starting inside a solid block would otherwise miss it entirely.
  {
    const BlockId b0 = w.get(c[0], c[1], c[2]);
    if (b0 != kAir) {
      hit[0] = c[0]; hit[1] = c[1]; hit[2] = c[2];
      prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];  // H8: never leave prev unset
      return true;
    }
  }
  float t = 0;
  while (t <= maxDist) {
    prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
    const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    c[a] += step[a]; t = tMax[a]; tMax[a] += tDelta[a];
    if (t > maxDist) break;
    const BlockId b = w.get(c[0], c[1], c[2]);
    if (b != kAir && blocks && blocks->has(b, kFlatPick)) {
      float y0, y1;
      flatPickRange(w, *blocks, c[0], c[1], c[2], y0, y1);
      const float lo[3] = {float(c[0]), y0, float(c[2])}, hi[3] = {float(c[0] + 1), y1, float(c[2] + 1)};
      if (!rayHitsBox(o, d, lo, hi, maxDist)) continue;  // looked through the air above / below the wire
    } else if (b != kAir && pickShape) {
      constexpr int kMaxBoxes = 16;
      PickBox boxes[kMaxBoxes];
      int n = pickShape(w, *blocks, c[0], c[1], c[2], b, boxes, kMaxBoxes);
      if (n > kMaxBoxes) n = kMaxBoxes;  // H8: never read past the box buffer
      if (n >= 0) {
        bool any = false;
        for (int i = 0; i < n && !any; ++i) {
          float lo[3], hi[3];
          for (int k = 0; k < 3; ++k) { lo[k] = float(c[k]) + boxes[i].lo[k]; hi[k] = float(c[k]) + boxes[i].hi[k]; }
          any = rayHitsBox(o, d, lo, hi, maxDist);
        }
        if (!any) continue;  // looked through the empty part of a partial block's cell
      }
    }
    if (b != kAir) { hit[0] = c[0]; hit[1] = c[1]; hit[2] = c[2]; return true; }
  }
  return false;
}

MarchHit marchRay(const ChunkMap& w, double ox, double oy, double oz, double dx, double dy, double dz,
                  double maxDist, const cBlockReg* blocks, bool skipOrigin) {
  MarchHit r;
  const double o[3] = {ox, oy, oz};
  double d[3] = {dx, dy, dz};
  for (int a = 0; a < 3; ++a)
    if (!(std::fabs(o[a]) < kMarchMaxCoord) || !std::isfinite(d[a])) return r;
  const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (!(len > 0.0) || !(maxDist > 0.0)) return r;
  for (double& v : d) v /= len;
  int c[3], step[3];
  double tMax[3], tDelta[3];
  for (int a = 0; a < 3; ++a) {
    c[a] = int(std::floor(o[a]));
    step[a] = d[a] > 0.0 ? 1 : -1;
    tDelta[a] = d[a] != 0.0 ? std::fabs(1.0 / d[a]) : 1e300;
    const double edge = d[a] > 0.0 ? double(c[a] + 1) - o[a] : o[a] - double(c[a]);
    tMax[a] = d[a] != 0.0 ? edge * tDelta[a] : 1e300;
  }
  auto solid = [&](BlockId b) { return blocks ? blocks->has(b, kSolid) : b != kAir; };
  int prev[3] = {c[0], c[1], c[2]};
  double t = 0.0;
  bool first = true;
  for (;;) {
    if (w.column(c[0] >> 4, c[2] >> 4) == nullptr) {
      r.stoppedByUnloaded = true;
      r.dist = t;
      for (int a = 0; a < 3; ++a) { r.cell[a] = c[a]; r.prev[a] = prev[a]; }
      return r;
    }
    if (!(first && skipOrigin) && solid(w.get(c[0], c[1], c[2]))) {
      r.hit = true;
      r.dist = t;
      for (int a = 0; a < 3; ++a) { r.cell[a] = c[a]; r.prev[a] = prev[a]; }
      return r;
    }
    first = false;
    const int a = tMax[0] <= tMax[1] ? (tMax[0] <= tMax[2] ? 0 : 2) : (tMax[1] <= tMax[2] ? 1 : 2);
    if (tMax[a] > maxDist) return r;
    for (int k = 0; k < 3; ++k) prev[k] = c[k];
    c[a] += step[a];
    t = tMax[a];
    tMax[a] += tDelta[a];
  }
}
}  // namespace litholattice
