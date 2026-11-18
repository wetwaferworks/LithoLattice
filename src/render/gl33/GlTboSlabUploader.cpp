#include "GlTboSlabUploader.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "litholattice/core/log.h"
#include "litholattice/render/unified_mesher.h"

namespace litholattice::gl33 {

bool cGlTboSlabUploader::prepare(const ChunkMesh& m) {
  CanonPullStream& ps = ps_;
  ps = CanonPullStream{};
  if (!m.canonical || !buildCanonPullStream(*m.canonical, ps)) return false;
  // Same quad set as the legacy stream or no deal (e.g. a mesh the game re-decoded).
  if (size_t(ps.fbQuads + ps.quads) * 4 != m.verts.size() || ps.texels.empty()) { ++rejects_; return false; }
  static const bool kCanonCheck = [] { const char* e = std::getenv("LL_GL_CANON_CHECK"); return e && e[0] == '1'; }();
  if (kCanonCheck) {  // diag: pulled quads vs the legacy stream the arrays path would draw
    using K = std::array<uint32_t, 10>;
    auto kv = [](const PackedVertex& v) { return K{v.x, v.y, v.z, v.u, v.v, v.faceAo, v.layer, v.value, v.mode, uint32_t(v.layerHi)}; };
    std::vector<std::array<K, 4>> a, b;
    for (uint32_t q = 0; q < ps.fbQuads + ps.quads; ++q) { std::array<K, 4> e; for (uint32_t c = 0; c < 4; ++c) e[c] = kv(canonPullReference(ps, q, c)); a.push_back(e); }
    for (size_t q = 0; q * 4 < m.verts.size(); ++q) { std::array<K, 4> e; for (int c = 0; c < 4; ++c) e[c] = kv(m.verts[q * 4 + c]); b.push_back(e); }
    static uint64_t ck = 0, bad = 0, ordBad = 0; ++ck;
    if (a != b) ++ordBad;
    auto sa = a, sb = b; std::sort(sa.begin(), sa.end()); std::sort(sb.begin(), sb.end());
    if (sa != sb) {
      if (bad++ < 6) for (size_t i = 0; i < sa.size(); ++i) if (sa[i] != sb[i]) {
        std::string s;
        for (int c = 0; c < 4; ++c) { s += " P["; for (uint32_t x : sa[i][c]) s += std::to_string(x) + ","; s += "] L["; for (uint32_t x : sb[i][c]) s += std::to_string(x) + ","; s += "]"; }
        logError("gl33 canon-check mesh %d,%d,%d quad %zu:%s", m.cx, m.sy, m.cz, i, s.c_str()); break; }
    }
    if ((ck & 1023) == 0 || bad == 1) logInfo("gl33 canon-check: %llu meshes, %llu set-mismatch, %llu order-diff", (unsigned long long)ck, (unsigned long long)bad, (unsigned long long)ordBad);
  }
  return true;
}

bool cGlTboSlabUploader::commit(MeshPlacement& out) {
  const size_t n = ps_.texels.size() / 4;
  const auto range = pool_.alloc(n);
  if (!range) return false;
  out.slab = range->slab; out.first = range->first; out.count = n;
  out.fallbackQuads = ps_.fbQuads; out.quads = ps_.quads;
  pool_.upload(*range, ps_.texels.data(), n);
  texels_ += n;
  ++meshes_;
  return true;
}

void cGlTboSlabUploader::release(const MeshPlacement& p) {
  pool_.release({p.slab, p.first}, p.count);
  texels_ -= p.count;
}

void cGlTboSlabUploader::clear() {
  pool_.reset();
  texels_ = 0;
}

}  // namespace litholattice::gl33
