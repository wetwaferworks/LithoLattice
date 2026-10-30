// Refactor step 6: see VkCanonUploader.h.
#include "VkCanonUploader.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <mutex>
#include "litholattice/render/canonical_quad.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/unified_mesher.h"

namespace litholattice::vk {

namespace {
constexpr uint64_t kOriginBits = 0x3FFFFull;               // canonical and pulled: ox | oy<<6 | oz<<12
constexpr uint64_t kPaletteBits = uint64_t(0x3FF) << 32;   // canonical hi 0-9
constexpr size_t kCacheCap = size_t(1) << 16;
}  // namespace

bool cVkCanonUploader::enabled() {
  static const bool on = [] {
    const char* e = std::getenv("LL_VK_CANON");
    const bool v = e && std::atoi(e) == 1;
    if (v) std::atexit(&cVkCanonUploader::printStats);
    return v;
  }();
  return on;
}

void cVkCanonUploader::printStats() {
  std::fprintf(stderr, "vk-canon: built %llu declined %llu checked %llu mismatches %llu tmpl hits %llu misses %llu\n",
               (unsigned long long)built_.load(), (unsigned long long)declined_.load(),
               (unsigned long long)checked_.load(), (unsigned long long)mismatches_.load(),
               (unsigned long long)hits_.load(), (unsigned long long)misses_.load());
}

void cVkCanonUploader::noteCheck(const PulledMesh& canon, const PulledMesh& ref) {
  checked_.fetch_add(1, std::memory_order_relaxed);
  if (sameLayout(canon, ref)) return;
  if (mismatches_.fetch_add(1, std::memory_order_relaxed) >= 4) return;  // describe the first few only
  std::fprintf(stderr, "vk-canon mismatch: fallback %zu/%zu palette %zu/%zu quads %zu/%zu buckets", canon.fallback.size(),
               ref.fallback.size(), canon.palette.size(), ref.palette.size(), canon.quads.size(), ref.quads.size());
  for (int f = 0; f < 7; ++f) std::fprintf(stderr, " %u/%u", canon.bucket[f], ref.bucket[f]);
  std::fprintf(stderr, "\n");
  const size_t nf = std::min(canon.fallback.size(), ref.fallback.size());
  for (size_t i = 0; i < nf; ++i) {
    const PackedVertex& x = canon.fallback[i]; const PackedVertex& y = ref.fallback[i];
    if (std::memcmp(&x, &y, sizeof(x)) != 0) {
      std::fprintf(stderr, "  fallback[%zu] canon xyz %u %u %u uv %u %u fa %u L %u v %u m %u | ref xyz %u %u %u uv %u %u fa %u L %u v %u m %u\n", i,
                   x.x, x.y, x.z, x.u, x.v, x.faceAo, x.layerIndex(), x.value, x.mode, y.x, y.y, y.z, y.u, y.v, y.faceAo,
                   y.layerIndex(), y.value, y.mode);
      break;
    }
  }
  const size_t nq = std::min(canon.quads.size(), ref.quads.size());
  for (size_t i = 0; i < nq; ++i)
    if (canon.quads[i] != ref.quads[i] || canon.quadMat[i] != ref.quadMat[i]) {
      std::fprintf(stderr, "  quad[%zu] canon %016llx m%u | ref %016llx m%u\n", i, (unsigned long long)canon.quads[i],
                   canon.quadMat[i], (unsigned long long)ref.quads[i], ref.quadMat[i]);
      break;
    }
}

bool cVkCanonUploader::decline() {
  declined_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

const cVkCanonUploader::Tmpl& cVkCanonUploader::lookup(uint64_t raw, uint64_t palEntry) {
  const Key key{raw & ~(kOriginBits | kPaletteBits), palEntry};
  if (auto it = cache_.find(key); it != cache_.end()) {
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }
  misses_.fetch_add(1, std::memory_order_relaxed);
  if (cache_.size() >= kCacheCap) cache_.clear();
  canon::DecodedQuad d = canon::unpackQuad(raw);
  // Archie review C4: face is a 3-bit field; 6/7 would index face[] out of bounds.
  if (size_t(d.face) >= 6) return cache_.emplace(key, Tmpl{}).first->second;
  CanonicalMesh one;
  one.palette.assign(1, palEntry);
  d.paletteIdx = 0;
  one.quads.assign(1, canon::packQuad(d));
  one.lods[0].face[size_t(d.face)] = BucketRange{0, 1};
  one_.clear();
  Tmpl t;
  if (decodeCanonicalMesh(one, 0, one_) && one_.size() == 4) {
    t.ok = true;
    uint64_t q = 0, e = 0;
    if (encodePulled(one_.data(), q, e)) {
      t.pulled = true;
      t.e = e;
      t.q = q & ~kOriginBits;
      const int po[3] = {int(q & 63u), int((q >> 6) & 63u), int((q >> 12) & 63u)};
      const int co[3] = {int(d.ox), int(d.oy), int(d.oz)};
      for (int i = 0; i < 3; ++i) t.delta[i] = po[i] - co[i];
    }
    for (int k = 0; k < 4; ++k) {
      t.local[k] = one_[size_t(k)];
      t.local[k].x = uint16_t(t.local[k].x - d.ox * 16);
      t.local[k].y = uint16_t(t.local[k].y - d.oy * 16);
      t.local[k].z = uint16_t(t.local[k].z - d.oz * 16);
    }
  }
  return cache_.emplace(key, t).first->second;
}

bool cVkCanonUploader::build(const ChunkMesh& m, uint32_t maxQuads, int blendMode, PulledMesh& out) {
  const CanonicalMesh* cm = m.canonical.get();
  if (!cm || cm->magic != canon::kMeshMagic || cm->next) return decline();
  const CanonicalLod& L = cm->lods[0];
  if (L.skirt.count) return decline();
  idx_.clear();
  auto range = [&](const BucketRange& r) {
    for (uint32_t i = r.start; i < r.start + r.count && i < cm->quads.size(); ++i) idx_.push_back(i);
  };
  for (int f = 0; f < 6; ++f) range(L.face[size_t(f)]);
  range(L.blend);
  const size_t fbVerts = cm->fallback.size();
  if ((fbVerts & 3u) || m.verts.size() != fbVerts + idx_.size() * 4 || m.verts.size() / 4 > maxQuads) return decline();

  out.fallback.clear(); out.palette.clear(); out.quads.clear(); out.quadMat.clear();
  pal_.clear();
  for (int f = 0; f < 6; ++f) { qs_[f].clear(); qm_[f].clear(); }
  auto addPulled = [&](uint64_t q, uint64_t e) {
    auto [it, added] = pal_.try_emplace(e, uint32_t(out.palette.size()));
    if (added) out.palette.push_back(e);
    const uint32_t face = uint32_t((q >> 30) & 3u) | uint32_t(((q >> 32) & 1u) << 2);
    qs_[face].push_back(q); qm_[face].push_back(it->second);
  };
  // Fallback prefix: same per-quad rule as buildPulled (these quads are not canonical until step 7).
  for (size_t i = 0; i < fbVerts; i += 4) {
    const PackedVertex* v = &m.verts[i];
    uint64_t q = 0, e = 0;
    const bool blendQuad = blendMode >= 0 && uint32_t(v[0].mode & 63) == uint32_t(blendMode);
    if (!blendQuad && encodePulled(v, q, e)) addPulled(q, e);
    else out.fallback.insert(out.fallback.end(), v, v + 4);
  }
  // Canonical quads: template + origin.
  for (uint32_t i : idx_) {
    const uint64_t raw = cm->quads[i];
    const canon::DecodedQuad d = canon::unpackQuad(raw);
    if (d.paletteIdx >= cm->palette.size()) return decline();
    const Tmpl& t = lookup(raw, cm->palette[d.paletteIdx]);
    if (!t.ok) return decline();
    const bool blendQuad = blendMode >= 0 && uint32_t(t.local[0].mode & 63) == uint32_t(blendMode);
    const int o[3] = {int(d.ox) + t.delta[0], int(d.oy) + t.delta[1], int(d.oz) + t.delta[2]};
    const bool inRange = o[0] >= 0 && o[0] < 64 && o[1] >= 0 && o[1] < 64 && o[2] >= 0 && o[2] < 64;
    if (t.pulled && !blendQuad && inRange) {
      addPulled(t.q | uint64_t(o[0]) | (uint64_t(o[1]) << 6) | (uint64_t(o[2]) << 12), t.e);
    } else {
      for (int k = 0; k < 4; ++k) {
        PackedVertex v = t.local[k];
        v.x = uint16_t(v.x + d.ox * 16); v.y = uint16_t(v.y + d.oy * 16); v.z = uint16_t(v.z + d.oz * 16);
        out.fallback.push_back(v);
      }
    }
  }
  for (uint32_t f = 0; f < 6; ++f) {
    out.bucket[f] = uint32_t(out.quads.size());
    out.quads.insert(out.quads.end(), qs_[f].begin(), qs_[f].end());
    out.quadMat.insert(out.quadMat.end(), qm_[f].begin(), qm_[f].end());
  }
  out.bucket[6] = uint32_t(out.quads.size());
  if (out.palette.size() + out.quads.size() > 32767u) return decline();  // buildPulled moves all to Packed12
  built_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool cVkCanonUploader::sameLayout(const PulledMesh& a, const PulledMesh& b) {
  if (a.fallback.size() != b.fallback.size() || a.palette != b.palette || a.quads != b.quads || a.quadMat != b.quadMat ||
      std::memcmp(a.bucket, b.bucket, sizeof(a.bucket)) != 0)
    return false;
  for (size_t i = 0; i < a.fallback.size(); ++i) {  // drawn as Packed12: compare what the GPU reads
    GpuVertex12 ga, gb;
    packVertex12(a.fallback[i], ga);
    packVertex12(b.fallback[i], gb);
    if (std::memcmp(&ga, &gb, sizeof(ga)) != 0) return false;
  }
  return true;
}

}  // namespace litholattice::vk