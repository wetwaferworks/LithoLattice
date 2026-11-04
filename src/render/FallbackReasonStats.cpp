#include "litholattice/render/FallbackReasonStats.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include "litholattice/render/renderer.h"

namespace litholattice {

void FallbackReasonStats::Local::add(FallbackReason r, uint32_t quads, uint8_t mode, const int ao[4]) {
  reasons[int(r)] += quads;
  faceFallback += quads;
  modes[mode & 63] += quads;
  if (r == FallbackReason::AoCorners) aoPatterns[(ao[0] & 3) | (ao[1] & 3) << 2 | (ao[2] & 3) << 4 | (ao[3] & 3) << 6] += quads;
}

FallbackReason FallbackReasonStats::checkCanonicalEligibility(bool slab, uint8_t mode, uint8_t rot, uint32_t slot,
                                                              bool uvDefault, const int ao[4]) noexcept {
  if (slab) return FallbackReason::NonUnitSubBlock;
  const bool greedyMode = mode == kShadeTex || mode == kShadeEmissive || mode == kShadeGroundPulse;
  if (!greedyMode) {
    if (mode == kShadeTraceTint || mode == kShadeStripTint0 || mode == kShadeStripTint1) return FallbackReason::TintOrPulse;
    return FallbackReason::ShadeMode;
  }
  if (rot != 0) return FallbackReason::FaceRotation;
  if (slot != 0) return FallbackReason::TintOrPulse;
  if (!uvDefault) return FallbackReason::UVOverride;
  if (!(ao[0] == ao[1] && ao[1] == ao[2] && ao[2] == ao[3])) return FallbackReason::AoCorners;
  return FallbackReason::Count;
}

FallbackReasonStats& FallbackReasonStats::instance() {
  static FallbackReasonStats s;
  return s;
}

void FallbackReasonStats::flush(Local& l, uint64_t totalFallbackQuads, uint64_t canonicalQuads) {
  const uint64_t rest = totalFallbackQuads > l.faceFallback ? totalFallbackQuads - l.faceFallback : 0;
  l.reasons[int(FallbackReason::CustomNormalOrShape)] += uint32_t(rest);
  for (int i = 0; i < kCount; ++i)
    if (l.reasons[i]) counts_[i].fetch_add(l.reasons[i], std::memory_order_relaxed);
  for (int i = 0; i < 64; ++i)
    if (l.modes[i]) modes_[i].fetch_add(l.modes[i], std::memory_order_relaxed);
  for (int i = 0; i < 256; ++i)
    if (l.aoPatterns[i]) ao_[i].fetch_add(l.aoPatterns[i], std::memory_order_relaxed);
  canonicalQuads_.fetch_add(canonicalQuads, std::memory_order_relaxed);
  canonFaces_.fetch_add(l.canonFaces, std::memory_order_relaxed);
  for (int i = 0; i < int(FallbackShape::Count); ++i)
    if (l.shapes[i]) shapes_[i].fetch_add(l.shapes[i], std::memory_order_relaxed);
  for (int i = 0; i < 256; ++i)
    if (l.customBlocks[i]) custom_[i].fetch_add(l.customBlocks[i], std::memory_order_relaxed);
  l = Local{};
}

std::string FallbackReasonStats::reasonsLine() const {
  static constexpr const char* kNames[kCount] = {"subblock", "rotation", "uv_override", "tint_pulse",
                                                 "custom_shape", "palette_limit", "ao_corners", "shade_mode"};
  uint64_t fb = 0;
  for (int i = 0; i < kCount; ++i) fb += counts_[i].load(std::memory_order_relaxed);
  const uint64_t canon = canonicalQuads_.load(std::memory_order_relaxed);
  const double denom = double(canon + fb ? canon + fb : 1), fd = double(fb ? fb : 1);
  char buf[160];
  std::snprintf(buf, sizeof buf, "[unified-mesher-reasons] canon=%" PRIu64 " (%.1f%%) fb_quads=%" PRIu64 " (%.1f%%)", canon,
                100.0 * double(canon) / denom, fb, 100.0 * double(fb) / denom);
  std::string s = buf;
  for (int i = 0; i < kCount; ++i) {
    const uint64_t c = counts_[i].load(std::memory_order_relaxed);
    std::snprintf(buf, sizeof buf, " %s=%" PRIu64 " (%.1f%%)", kNames[i], c, 100.0 * double(c) / fd);
    s += buf;
  }
  std::snprintf(buf, sizeof buf, " | canon_faces=%" PRIu64, canonFaces_.load(std::memory_order_relaxed));
  return s + buf;
}

std::string FallbackReasonStats::detailLine() const {
  // Top per-face shade modes and AO patterns among fallback faces.
  static constexpr const char* kShapeNames[int(FallbackShape::Count)] = {"box_model", "shaped", "custom", "cross", "extra"};
  std::string s = "[unified-mesher-reasons-detail] custom_shape:";
  {
    char b2[64];
    for (int i = 0; i < int(FallbackShape::Count); ++i) {
      std::snprintf(b2, sizeof b2, " %s=%" PRIu64, kShapeNames[i], shapes_[i].load());
      s += b2;
    }
  }
  {
    int cidx[256];
    for (int i = 0; i < 256; ++i) cidx[i] = i;
    std::sort(cidx, cidx + 256, [&](int a, int b) { return custom_[a].load() > custom_[b].load(); });
    s += " | custom_by_block:";
    char b3[48];
    for (int k = 0; k < 6 && custom_[cidx[k]].load(); ++k) {
      std::snprintf(b3, sizeof b3, " id%d=%" PRIu64, cidx[k], custom_[cidx[k]].load());
      s += b3;
    }
  }
  s += " | modes:";
  int idx[64];
  for (int i = 0; i < 64; ++i) idx[i] = i;
  std::sort(idx, idx + 64, [&](int a, int b) { return modes_[a].load() > modes_[b].load(); });
  char buf[96];
  for (int k = 0; k < 5 && modes_[idx[k]].load(); ++k) {
    std::snprintf(buf, sizeof buf, " mode%d=%" PRIu64, idx[k], modes_[idx[k]].load());
    s += buf;
  }
  int aidx[256];  // Archie review M5: was static (shared across concurrent callers)
  for (int i = 0; i < 256; ++i) aidx[i] = i;
  std::sort(aidx, aidx + 256, [&](int a, int b) { return ao_[a].load() > ao_[b].load(); });
  s += " | ao(c0,c1,c2,c3):";
  for (int k = 0; k < 4 && ao_[aidx[k]].load(); ++k) {
    const int p = aidx[k];
    std::snprintf(buf, sizeof buf, " %d%d%d%d=%" PRIu64, p & 3, p >> 2 & 3, p >> 4 & 3, p >> 6 & 3, ao_[p].load());
    s += buf;
  }
  return s;
}

}  // namespace litholattice
