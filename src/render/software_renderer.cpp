// Cortana 2026-10-15 1010-1116: RND-H4/H3/H2, GAME-H1, RND-MT band raster
// Software renderer: full CPU rasterizer implementation.
// Optimized for voxels: backface culling, depth buffer, scanline rasterization.
// Shading: flat face colors, or per-pixel fragment shaders (basic / PBR tiers)
// sampling the CPU texture array. For baseline testing and GPU-less platforms.
// Implements IRenderer (mesh stores + renderFrame) on top of the same direct
// renderMesh path the headless consumers have always driven.
#include "litholattice/render/software_renderer.h"
#include "litholattice/core/bitops.h"
#include "litholattice/core/threeway.h"  // Design B Stage 1: detached renderFrame runs under RendererGuard, like RenderWorker::loop
#include "font_data.h"  // Cortana 2026-10-15 1610: Debug Overlay strip glyphs
#include "litholattice/render/model_dither.h"
#include "litholattice/render/software_aa.h"
#include "litholattice/render/software_ascii.h"
#include "litholattice/render/software_shaders.h"
#include "litholattice/core/log.h"
#include "litholattice/render/ground_pulse.h"
#include "litholattice/render/ground_traces.h"
#include <chrono>
#include "litholattice/render/software_simd.h"
#include <exception>
#include "litholattice/render/trace_ramps.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <climits>
#include <thread>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <algorithm>
#include "litholattice/render/block_materials.h"
#include "litholattice/render/texture_coverage.h"
#include <string_view>
#include <unordered_map>
#include <utility>
#include <cmath>
#include <limits>  // Cortana 2026-10-15 1715: Hi-Z
#include <cstring>

#include <cstdlib>
static int llP2PerfOff() { static const int v = [] { const char* e = std::getenv("LL_P2_PERF_OFF"); return e ? int(std::strtol(e, nullptr, 0)) : 0; }(); return v; }  // Cortana 2026-10-16 1520: A/B perf switch (render/display only)

namespace litholattice {

// 2026-10-16 crystal twinkle: 255 * max(0, sin(2*pi*i/256))^3 * 0.8, precomputed (no libm at
// run time). Index = (render-clock phase + per-crystal G) & 255.
static constexpr uint8_t kTwinkleLut[256] = {0,0,0,0,0,0,1,1,2,2,3,4,5,6,8,10,11,14,16,19,21,24,28,31,35,39,43,48,52,57,62,67,72,77,83,89,94,100,106,111,117,123,129,134,140,145,151,156,161,166,170,175,179,183,186,189,192,195,197,199,201,202,203,204,204,204,203,202,201,199,197,195,192,189,186,183,179,175,170,166,161,156,151,145,140,134,129,123,117,111,106,100,94,89,83,77,72,67,62,57,52,48,43,39,35,31,28,24,21,19,16,14,11,10,8,6,5,4,3,2,2,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

// ---------------------------------------------------------------------------
// cCpuTextureArray
// ---------------------------------------------------------------------------

bool cCpuTextureArray::upload(const cTextureReg& textures) {
  const std::span<const Texture> src = textures.textures();
  layers_ = int(src.size());
  // Cortana 2026-10-16 (BUG-0003): keep the pack's own resolution (was a fixed 16x16 box
  // downsample that averaged 4-16 source pixels per texel = the software-renderer blur).
  int cap = maxTile_;
  if (const char* e = std::getenv("LL_SW_TEXRES")) cap = std::atoi(e);
  if (cap != 16 && cap != 32 && cap != 64)
    cap = (std::thread::hardware_concurrency() != 0 && std::thread::hardware_concurrency() <= 4) ? 32 : 64;
  int srcMax = kTileSize;
  for (const Texture& t : src) srcMax = std::max(srcMax, t.image.width());
  tile_ = srcMax >= 64 && cap >= 64 ? 64 : srcMax >= 32 && cap >= 32 ? 32 : 16;
  const int kT = tile_, kA = kT * kT;
  texels_.assign(size_t(layers_) * size_t(kA), 0xFFFF00FFu);  // magenta = missing
  for (int layer = 0; layer < layers_; ++layer) {
    const Image& img = src[size_t(layer)].image;
    if (img.empty() || img.width() <= 0 || img.height() <= 0) continue;
    uint32_t* dst = &texels_[size_t(layer) * size_t(kA)];
    const std::string_view lname = src[size_t(layer)].name;
    const bool glow = lname.find("leaves") != std::string_view::npos && lname.find("_glow") != std::string_view::npos;
    const bool cover = coverageMipLayer(lname);
    // Box filter to 16x16 (identity when the source tile already is 16x16):
    // the same bounds and per-channel integer averaging biome_capture applies
    // when it builds the software texture buffer by hand.
    for (int y = 0; y < kT; ++y) {
      const int y0 = y * img.height() / kT;
      const int y1 = std::max(y0 + 1, (y + 1) * img.height() / kT);
      for (int x = 0; x < kT; ++x) {
        const int x0 = x * img.width() / kT;
        const int x1 = std::max(x0 + 1, (x + 1) * img.width() / kT);
        uint32_t sum[4] = {0, 0, 0, 0};
        int n = 0;
        for (int sy = y0; sy < y1; ++sy)
          for (int sx = x0; sx < x1; ++sx, ++n) {
            const uint8_t* p = img.pixel(sx, sy);
            for (int k = 0; k < 4; ++k) sum[k] += p[k];
          }
        const uint32_t r = n ? sum[0] / uint32_t(n) : 0, g = n ? sum[1] / uint32_t(n) : 0,
                       b = n ? sum[2] / uint32_t(n) : 0, a = n ? sum[3] / uint32_t(n) : 0xFF;
        dst[size_t(y) * kT + size_t(x)] = (a << 24) | (r << 16) | (g << 8) | b;
        if (glow) {  // 2026-10-16: glow masks hold strength/phase, not colour: nearest, never averaged
          const uint8_t* p = img.pixel(x0, y0);
          dst[size_t(y) * kT + size_t(x)] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
      }
    }
    if (cover && img.width() > kT) {  // 2026-10-16 (JD leaf pass): keep level-0 alpha-test coverage
      std::vector<uint8_t> rgba(size_t(kA) * 4);
      for (int i = 0; i < kA; ++i) rgba[size_t(i) * 4 + 3] = uint8_t(dst[i] >> 24);
      int pass = 0;
      for (int sy = 0; sy < img.height(); ++sy)
        for (int sx = 0; sx < img.width(); ++sx) pass += img.pixel(sx, sy)[3] >= 128;
      preserveAlphaCoverage(rgba.data(), kT, kT, float(pass) / float(img.width() * img.height()));
      for (int i = 0; i < kA; ++i) dst[i] = (dst[i] & 0x00FFFFFFu) | (uint32_t(rgba[size_t(i) * 4 + 3]) << 24);
    }
  }
  // 2026-10-16 (JD bug 1): keep the palette rows (row 0 of each palette image).
  palettes_.assign(textures.palettes().size() * kPaletteWidth, 0xFF000000u);
  for (size_t r = 0; r < textures.palettes().size(); ++r) {
    const auto& cs = textures.palettes()[r].colors;
    for (size_t i = 0; i < cs.size() && i < size_t(kPaletteWidth); ++i)
      palettes_[r * kPaletteWidth + i] = 0xFF000000u | (uint32_t(cs[i][0]) << 16) | (uint32_t(cs[i][1]) << 8) | cs[i][2];
  }
  buildMorton();
  buildMips();  // Cortana 2026-10-16 (BUG-0005)
  return true;
}

// Ginny (Argon) 2026-10-16 Pass 2 step 4: swizzle every 16x16 layer to Morton order at load.
// Cortana 2026-10-16 (BUG-0005): 2x2 box mips for 32/64 tiles so distant faces sample a
// texel density close to 1 per pixel instead of aliasing (sparkle / incoherent noise).
void cCpuTextureArray::buildMips() {
  levels_ = 1;
  for (auto& m : mips_) m.clear();
  if (tile_ <= 16 || layers_ <= 0) return;
  const uint32_t* prev = texels_.data();
  int pt = tile_;
  for (int k = 1; k < kMaxMips && (pt >> 1) >= 4; ++k) {
    const int t = pt >> 1;
    mips_[k].assign(size_t(layers_) * size_t(t * t), 0u);
    for (int L = 0; L < layers_; ++L) {
      const uint32_t* s = prev + size_t(L) * size_t(pt * pt);
      uint32_t* d = mips_[k].data() + size_t(L) * size_t(t * t);
      for (int y = 0; y < t; ++y)
        for (int x = 0; x < t; ++x) {
          const uint32_t q[4] = {s[(2 * y) * pt + 2 * x], s[(2 * y) * pt + 2 * x + 1], s[(2 * y + 1) * pt + 2 * x], s[(2 * y + 1) * pt + 2 * x + 1]};
          uint32_t o = 0;
          for (int sh = 0; sh < 32; sh += 8) {
            uint32_t sum = 0;
            for (uint32_t v : q) sum += (v >> sh) & 0xFF;
            o |= ((sum + 2) / 4) << sh;
          }
          d[y * t + x] = o;
        }
    }
    prev = mips_[k].data();
    pt = t;
    levels_ = k + 1;
  }
}

void cCpuTextureArray::buildMorton() {
  if (tile_ != 16) { texelsMorton_.clear(); return; }  // Cortana 2026-10-16: Morton tables are 16x16
  texelsMorton_.assign(texels_.size(), 0u);
  for (size_t l = 0; l < texels_.size() / 256; ++l)
    for (uint32_t ty = 0; ty < 16; ++ty)
      for (uint32_t tx = 0; tx < 16; ++tx)
        texelsMorton_[l * 256 + (kSwMortonU[tx] | kSwMortonV[ty])] = texels_[l * 256 + ty * 16 + tx];
}

void cCpuTextureArray::setRaw(const uint32_t* data, uint32_t layers) {
  if (!data || layers == 0) {
    texels_.clear();
    texelsMorton_.clear();
    layers_ = 0;
    return;
  }
  layers_ = int(layers);
  tile_ = kTileSize;  // raw uploads are 16x16 tiles
  levels_ = 1;
  texels_.assign(data, data + size_t(layers) * 256);
  buildMorton();
}

// ---------------------------------------------------------------------------
// cSoftwareRenderer
// ---------------------------------------------------------------------------

cSoftwareRenderer::cSoftwareRenderer() {
  fb_.resize(800, 600);
  depth_.resize(size_t(800) * 600, 1.0f);
}

cSoftwareRenderer::~cSoftwareRenderer() = default;

void cSoftwareRenderer::resize(uint32_t w, uint32_t h) {
  fb_.resize(w, h);
  depth_.assign(size_t(w) * h, 1.0f);
  hizFbW_ = 0;  // Cortana 2026-10-15 1715: Hi-Z off until the next clear()
}

thread_local cSoftwareRenderer::GroundFx cSoftwareRenderer::groundFx_{};
thread_local float cSoftwareRenderer::meshOriginX_ = 0.f;
thread_local float cSoftwareRenderer::meshOriginY_ = 0.f;
thread_local float cSoftwareRenderer::meshOriginZ_ = 0.f;

// RND-H3 banding (2026-10-15): the rows this thread may write. The full
// frame by default; the chunk pass narrows it per band thread.
static thread_local int tlBandY0 = 0;
static thread_local int tlBandY1 = 1 << 30;
static thread_local int tlBandX0 = 0;        // 2026-10-25 step 1: heavy strips split into x tiles
static thread_local int tlBandX1 = 1 << 30;
// Cortana 2026-10-15 1112: pre-transformed screen verts for the mesh the
// band thread is drawing (set by the chunk pass; null = transform here).
static thread_local const float* tlPreScreen = nullptr;
// Cortana 2026-10-15 1255: optional quad-index list for the mesh being drawn
// (the strip's bin); null = every quad. Indices are quad numbers (verts/4)
// in ascending order, so per-strip draw order is the original order.
static thread_local const uint32_t* tlQuadList = nullptr;
static thread_local size_t tlQuadCount = 0;
// Cortana 2026-10-15 1325: back-face culling for the chunk pass, matching
// the GL/Vulkan chunk pipelines (GL_CULL_FACE, GL_BACK, CCW front).
// Sign of the screen-space area of a back face (y-down screen flips CCW).
static thread_local bool tlCullBack = false;
// Cortana 2026-10-15 1350: per-quad shading mode for the per-pixel path
// (set by renderMesh before rasterizing a quad). Cutout + Trace strip modes
// alpha-test at 0.5 (the "black box" around Trace was alpha-0 texels drawn
// opaque), and strip modes tint the gray mask through the r3 power ramps.
static thread_local uint8_t tlQuadMode = 0;
static thread_local uint8_t tlQuadValue = 0;
// Cortana 2026-10-15 1755 [ENGINE]: blended chunk pass filter for renderMesh's per-pixel
// path: -1 every quad, 0 opaque quads only, 1 kShadeAlphaBlend quads only.
static thread_local int8_t tlBlendPass = -1;
// Cortana 2026-10-15 1345: sign verified on real walk frames (no holes vs cull-off).
static constexpr float g_backSign = 1.0f;
// Ginny (Argon) 2026-10-16 Pass 2 feature bits (Cortana). LL_SW_P2=<mask> overrides the
// default (e.g. LL_SW_P2=0 = the pre-Pass-2 renderer). LL_SW_AB=<frame> renders that frame
// twice, Pass 2 off then on, and logs the pixel diff (A/B identity proof on real frames).
enum : unsigned { kP2Quad = 1u, kP2Par = 2u, kP2FaceCull = 4u, kP2Morton = 8u, kP2Dispatch = 16u, kP2SpanClip = 32u, kP2EmitMemo = 64u, kP2CentreCull = 128u };  // kP2SpanClip: Cortana 2026-10-25 box prototype, opt-in
static constexpr unsigned kP2Default = kP2Quad | kP2Par | kP2FaceCull | kP2EmitMemo | kP2CentreCull;  // Cortana 2026-10-25 box: +EmitMemo, +CentreCull (bit-identical, LL_SW_AB 0 px)
//  // kP2Morton (16x16 tiles already sit in L1) and kP2Dispatch: opt-in, byte-identical, no measurable gain
static std::atomic<unsigned> g_p2{[] {
  const char* e = std::getenv("LL_SW_P2");
  return e && *e ? unsigned(std::strtoul(e, nullptr, 0)) : kP2Default;
}()};
static inline bool swP2(unsigned bit) { return (g_p2.load(std::memory_order_relaxed) & bit) != 0; }
static const bool g_swProfOn = std::getenv("LL_SW_PROF") != nullptr;
static double g_hudMsAcc = 0, g_hudBuildMs = 0; static uint64_t g_hudTris = 0, g_hudFlat = 0, g_hudBox = 0;  // 2026-10-25 HUD split (LL_SW_PROF)
static std::atomic<uint64_t> g_pxScan{0}, g_pxCov{0}, g_pxShade{0}, g_tris{0};  // LL_SW_PROF counters
static std::atomic<uint64_t> g_hizRows{0};
static std::atomic<uint64_t> g_centreCulled{0}, g_emitMemoHits{0}, g_quadCentreCulled{0};
static thread_local uint64_t tlQuadCentreCulled = 0;  // 2026-10-25 step 2: flushed per band  // Cortana 2026-10-25 box: tris dropped by kP2CentreCull
// Cortana 2026-10-25 (pass sw2, Archie): LL_SW_PROF extras - rows visited by the span loop
// (Hi-Z rejected / total), SIMD-span vs scalar-span pixels, and per-thread strip time spread.
static std::atomic<uint64_t> g_rowsTotal{0}, g_pxSimd{0}, g_pxScalar{0};
static double g_bandMs[64] = {};
static double g_thrMinAcc = 0, g_thrMaxAcc = 0;
static std::atomic<uint64_t> g_fcCulled{0}, g_fcQuads{0};  // Pass 2 step 3 (LL_SW_PROF): quads culled by face bucket / total  // Cortana 2026-10-15 1715: rows skipped by Hi-Z (LL_SW_PROF)

namespace {
// Persistent band threads for the chunk pass (no per-frame thread creation).
// Cortana 2026-10-15 1156: thread count. Default 4 (JD: entry/older consumer
// hardware); override with cSoftwareRenderer::setRenderThreads (CLI
// --render-threads, settings key renderThreads). Always a power of two,
// clamped to <= hardware threads, capped at kSwMaxRenderThreads, >= 1.
std::atomic<unsigned> g_swRequestedThreads{0};  // 0 = default
[[maybe_unused]] constexpr unsigned kSwDefaultRenderThreads = 4;  // superseded 1450: auto (swAutoRenderThreads); kept for reference
constexpr unsigned kSwMaxRenderThreads = 64;
// Cortana 2026-10-15 1450 (ENGINE, JD ruling 14:41): default = auto by cores.
// The largest power of two <= (logical cores - 2, which are left for the sim
// and main threads), clamped to 2..16. i5-12450H (12 logical) gives 8.
// An explicit request (CLI / settings renderThreads != 0) still overrides.
unsigned swAutoRenderThreads(unsigned logical) {
  // 2026-10-25 (Archie step 1): strips are pulled dynamically, so no power-of-two need:
  // std::clamp(hw - 1, 2, 16). i5-12450H (12 logical) gives 11.
  return std::clamp(logical > 1 ? logical - 1 : 1u, 2u, 16u);
}
unsigned swResolveThreads(unsigned req) {
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  if (!req) return swAutoRenderThreads(hw);  // auto: not limited by the hw clamp below (2-core floor is 2 by ruling)
  const unsigned want = std::min(req, kSwMaxRenderThreads);
  unsigned b = 1;
  while (b * 2 <= want && b * 2 <= hw) b *= 2;
  return b;
}
class cSwBandPool {
 public:
  static cSwBandPool& get() { static cSwBandPool p; return p; }
  unsigned bands() const { return bands_; }
  void run(const std::function<void(unsigned)>& fn) {
    // Cortana 2026-10-15 1415 (Elfie §1.4): nested run() from a band
    // callback (or any thread already inside run) used to deadlock on runM_.
    // It now runs every band inline on the calling thread - same result,
    // no re-entry into the pool.
    if (tlInRun_) { for (unsigned b = 0; b < std::max(1u, bands_); ++b) fn(b); return; }
    struct InRun { InRun() { tlInRun_ = true; } ~InRun() { tlInRun_ = false; } } inRun;
    // Serialize whole frames (game view + MCP vision share the pool) and
    // apply a thread-count change between frames.
    std::lock_guard<std::mutex> runLock(runM_);
    // Design B Stage 1: a detached frame in flight owns the pool's frame
    // slot; a synchronous frame waits for it (the design's §4.3 rule: vision
    // serialises behind an in-flight detached frame, as it does today via
    // runM_). runM_ alone cannot express this — its owner would have to be
    // the executor thread — so frame ownership is a flag under m_. The
    // executor running a detached body IS the frame owner (tlDetachedBody_):
    // its run() calls skip the wait and the slot release, which stay with
    // runDetached/executorLoop for the body's whole duration.
    if (!tlDetachedBody_) {
      { std::unique_lock<std::mutex> l(m_); frameFree_.wait(l, [&] { return !frameActive_; }); frameActive_ = true; }
    }
    struct FrameSlot {
      cSwBandPool& p;
      bool held;
      ~FrameSlot() { if (held) { { std::lock_guard<std::mutex> l(p.m_); p.frameActive_ = false; } p.frameFree_.notify_all(); } }
    } frameSlot{*this, !tlDetachedBody_};
    const unsigned want = swResolveThreads(g_swRequestedThreads.load(std::memory_order_relaxed));
    // Design B Stage 1: the executor cannot restart the pool it belongs to
    // (stopAll() would self-join); runDetached applies the change between
    // frames instead — it re-checks on every submit.
    if (want != bands_ && !tlDetachedBody_) { stopAll(); start(want); }
    if (bands_ <= 1) { fn(0); return; }
    // Several cSoftwareRenderer instances share the pool (the game view and
    // the MCP vision capture for the Daemon's pilots): one frame at a time.
    // run() returns only after every band has finished, so the framebuffer
    // is fully resolved before any post pass, grabFramebuffer or capture.
    {
      std::lock_guard<std::mutex> l(m_);
      fn_ = &fn;
      remaining_ = bands_ - 1;
      ++gen_;
    }
    cv_.notify_all();
    // Cortana 2026-10-15 1415: exception guard. A throwing band no longer
    // skips its remaining_ decrement (which hung run() forever); the first
    // exception is rethrown here after EVERY band finished, so the
    // framebuffer is never torn down under a running worker.
    std::exception_ptr mine;
    try { fn(0); } catch (...) { mine = std::current_exception(); }
    std::unique_lock<std::mutex> l(m_);
    done_.wait(l, [&] { return remaining_ == 0; });
    fn_ = nullptr;
    std::exception_ptr err = mine ? mine : workerErr_;
    workerErr_ = nullptr;
    l.unlock();
    if (err) std::rethrow_exception(err);
  }
  // ---- Design B Stage 1: detached submit mode (JD ruling 2: the render
  // side rides the band-pool threads; no new always-on thread outside the
  // pool). While detachedMode_ is on, start() spawns bands_ workers: the
  // band workers 1..bands_-1 exactly as before, plus the executor (band
  // 0's seat), whose only work is runDetached bodies. The band count is
  // unchanged — detached mode changes WHO runs a detached frame (the
  // executor instead of the submitting caller), never how many bands a
  // frame has. The executor runs the body as the frame's caller (band
  // 0's seat): the body's run() calls fork-join normally, and a run()
  // re-entered from inside a band callback still takes the tlInRun_
  // inline path (the Daemon/MCP-vision fallback, preserved verbatim);
  // a synchronous run() from any other thread keeps
  // caller-runs-band-0 over the band workers and serialises against an
  // in-flight detached frame through frameActive_ (see run()).
  void setDetachedMode(bool on) {
    std::lock_guard<std::mutex> runLock(runM_);
    { std::unique_lock<std::mutex> l(m_); frameFree_.wait(l, [&] { return !frameActive_; }); }
    if (detachedMode_ == on) return;
    detachedMode_ = on;
    stopAll();
    start(bands_);
  }
  bool detachedMode() const { return detachedMode_; }
  // Publish fn to the executor and return immediately. fn runs on the
  // executor; onDone(err) runs there right after it (err null on success).
  // One detached job at a time: the mailbox guarantees the previous job
  // completed before submitting again, and frameActive_ serialises
  // against synchronous run() frames either way. If detached mode is off
  // there is no executor: fn runs inline on the caller and onDone fires
  // before return (the Stage 0 shape), so callers stay correct.
  void runDetached(std::function<void()> fn, std::function<void(std::exception_ptr)> onDone) {
    std::unique_lock<std::mutex> runLock(runM_);
    { std::unique_lock<std::mutex> l(m_); frameFree_.wait(l, [&] { return !frameActive_; }); frameActive_ = true; }
    // Thread-count changes apply between frames, as in run().
    const unsigned want = swResolveThreads(g_swRequestedThreads.load(std::memory_order_relaxed));
    if (want != bands_) { stopAll(); start(want); }
    if (!detachedMode_) {
      std::exception_ptr err;
      try { fn(); } catch (...) { err = std::current_exception(); }
      { std::lock_guard<std::mutex> l(m_); frameActive_ = false; }
      frameFree_.notify_all();
      if (onDone) onDone(err);
      return;
    }
    { std::lock_guard<std::mutex> l(m_); detachedFn_ = std::move(fn); detachedDone_ = std::move(onDone); }
    runLock.unlock();
    cv_.notify_all();
  }
  ~cSwBandPool() { stopAll(); }
 private:
  cSwBandPool() {
    std::fprintf(stderr, "software renderer: SIMD path %s\n", swsimd::activePath());  // Cortana 2026-10-15 1455
    start(swResolveThreads(g_swRequestedThreads.load()));
  }
  void stopAll() {
    { std::lock_guard<std::mutex> l(m_); stop_ = true; ++gen_; }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    std::lock_guard<std::mutex> l(m_);
    stop_ = false;
  }
  void start(unsigned n) {
    bands_ = n;
    {  // Ginny (Argon) 2026-10-16 Pass 2 step 2: thread-count telemetry (oversubscription / NUMA check)
      const unsigned hw = std::thread::hardware_concurrency();
      const unsigned spawned = detachedMode_ ? n : (n ? n - 1 : 0);
      std::fprintf(stderr, "software renderer: hardware_concurrency=%u, render bands=%u (%u spawned workers%s)\n", hw, n, spawned, detachedMode_ ? " incl. detached executor" : " + caller");
      logInfo("software renderer: hardware_concurrency=%u, render bands=%u (%u spawned workers%s)", hw, n, spawned, detachedMode_ ? " incl. detached executor" : " + caller");
    }
    uint64_t g;
    { std::lock_guard<std::mutex> l(m_); g = gen_; }
    if (detachedMode_) threads_.emplace_back([this] { executorLoop(); });
    for (unsigned i = 1; i < bands_; ++i) threads_.emplace_back([this, i, g] { loop(i, g); });
  }
  // Design B Stage 1: the executor worker. Waits for a published detached
  // body and runs it AS THE FRAME'S CALLER: the executor takes band 0's
  // seat, so the body's own run() calls fork-join over the band workers
  // exactly as a synchronous frame's do (runM_ serialises them against
  // any other frame; the body's band callbacks run under tlInRun_ as
  // always, so a run() re-entered from inside a band callback still
  // inlines — the Daemon/MCP-vision fallback is preserved verbatim).
  // tlDetachedBody_ marks this thread as the frame owner runDetached
  // already accounted for. The completion callback runs here, before the
  // frame slot is released, so a following run()/runDetached can never
  // overlap the completion bookkeeping; it must not throw (a throw is
  // swallowed rather than killing the pool's only executor).
  void executorLoop() {
    for (;;) {
      std::function<void()> fn;
      std::function<void(std::exception_ptr)> done;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return stop_ || detachedFn_ != nullptr; });
        if (stop_) return;
        fn = std::move(detachedFn_); detachedFn_ = nullptr;
        done = std::move(detachedDone_); detachedDone_ = nullptr;
      }
      tlDetachedBody_ = true;
      std::exception_ptr err;
      try { fn(); } catch (...) { err = std::current_exception(); }
      tlDetachedBody_ = false;
      if (done) { try { done(err); } catch (...) {} }
      { std::lock_guard<std::mutex> l(m_); frameActive_ = false; }
      frameFree_.notify_all();
    }
  }
  void loop(unsigned band, uint64_t seen) {
    for (;;) {
      const std::function<void(unsigned)>* fn = nullptr;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return stop_ || gen_ != seen; });
        if (stop_) return;
        seen = gen_;
        fn = fn_;
      }
      tlInRun_ = true;  // a band callback calling run() takes the inline path
      std::exception_ptr err;
      try { (*fn)(band); } catch (...) { err = std::current_exception(); }
      std::lock_guard<std::mutex> l(m_);
      if (err && !workerErr_) workerErr_ = err;
      if (--remaining_ == 0) done_.notify_one();
    }
  }
  unsigned bands_ = 1;
  std::vector<std::thread> threads_;
  std::mutex m_, runM_;
  std::condition_variable cv_, done_;
  const std::function<void(unsigned)>* fn_ = nullptr;
  unsigned remaining_ = 0;
  std::exception_ptr workerErr_;  // first worker exception of the current run (m_)
  // Design B Stage 1 (detached mode): all under m_ unless noted.
  bool detachedMode_ = false;                 // start() spawns the executor while true
  bool frameActive_ = false;                  // a run() frame or a detached job owns the pool
  std::condition_variable frameFree_;         // signalled when frameActive_ clears
  std::function<void()> detachedFn_;          // the published detached body (executor's only)
  std::function<void(std::exception_ptr)> detachedDone_;  // its completion callback
  inline static thread_local bool tlInRun_ = false;
  inline static thread_local bool tlDetachedBody_ = false;  // Design B Stage 1: this thread runs a detached body (frame owner)
  uint64_t gen_ = 0;
  bool stop_ = false;
};
}  // namespace

void cSoftwareRenderer::setRenderThreads(unsigned n) { g_swRequestedThreads.store(n); }
static std::atomic<float> g_swRenderScale{1.0f};  // pass sw3: --sw-scale
void cSoftwareRenderer::setRenderScale(float s) { g_swRenderScale.store(std::clamp(s, 0.25f, 1.0f)); }
unsigned cSoftwareRenderer::renderThreads() { return swResolveThreads(g_swRequestedThreads.load()); }

// Design B Stage 2: forward declaration (defined with the store, below) +
// the (store key, ring kind) map key for drain coalescing.
static int64_t swMeshKey(int cx, int sy, int cz);
struct PairHash {
  size_t operator()(const std::pair<int64_t, int>& p) const noexcept {
    return std::hash<int64_t>{}(p.first * 2 + p.second);
  }
};

// Design B Stage 1: detached frame execution for the software threeway
// split. The body runs on the band pool's executor worker; the
// RendererGuard mirrors RenderWorker::loop on the Vulkan path, so the
// game's remaining guarded renderer calls (which take the same guard)
// stay serialised with the in-flight render.
//
// Design B Stage 2: the body opens with the frame seal — the upload ring
// drains (drainUploadRing) BEFORE the guard is taken and before
// renderFrame builds its draw order. The drain takes no lock and touches
// nothing but the ring and the store, which are executor-owned between
// seals: on this path the game's upload phase never calls the renderer
// and never takes the guard, so the upload rendezvous Stage 1 measured
// is gone. Drained source buffers are retired after the render (Argon
// trap 3), even when the render throws.
void cSoftwareRenderer::renderFrameDetached(const FrameParams& fp, uint64_t drainTarget,
                                           const std::function<void(std::exception_ptr)>& onDone) {
  cSwBandPool::get().runDetached(
      [this, fp, drainTarget] {
        std::exception_ptr err;
        try {
          drainUploadRing(drainTarget);  // the seal (no-op unless the ring is enabled)
          litholattice::threeway::RendererGuard rg;
          renderFrame(fp);
        } catch (...) {
          err = std::current_exception();
        }
        retireDrainedMeshes();  // frees happen here, off the seal, throw or not
        if (err) std::rethrow_exception(err);
      },
      onDone);
}

// ---- Design B Stage 2: the upload ring (producer side) -------------------
// These run on the main thread (the game's upload phase). While the ring
// is disabled they are the direct calls they replace, byte for byte.

bool cSoftwareRenderer::enqueueChunkMesh(ChunkMesh&& mesh) {
  if (!uploadRingEnabled_) { uploadChunkMesh(mesh); return true; }
  const bool tombstone = mesh.verts.empty();  // removals are must-push: never refused for bytes
  cSwUploadRing::Slot* s = uploadRing_.claim(uint32_t(mesh.verts.size() * sizeof(PackedVertex)), tombstone);
  if (!s) return false;  // ring full: caller keeps the mesh (spill/retry) — never dropped
  s->kind = cSwUploadRing::Kind::Chunk;
  s->mesh = std::move(mesh);
  uploadRing_.publish();
  return true;
}

bool cSoftwareRenderer::enqueuePbrChunkMesh(PbrChunkMesh&& mesh) {
  if (!uploadRingEnabled_) { uploadPbrChunkMesh(mesh); return true; }
  const bool tombstone = mesh.verts.empty();
  cSwUploadRing::Slot* s = uploadRing_.claim(uint32_t(mesh.verts.size() * sizeof(PbrQuadVertex)), tombstone);
  if (!s) return false;
  s->kind = cSwUploadRing::Kind::PbrChunk;
  s->pbrMesh = std::move(mesh);
  uploadRing_.publish();
  return true;
}

bool cSoftwareRenderer::enqueueClearChunkMeshes() {
  if (!uploadRingEnabled_) { clearChunkMeshes(); return true; }
  cSwUploadRing::Slot* s = uploadRing_.claim(0, /*mustPush=*/true);  // a clear is never dropped
  if (!s) return false;
  s->kind = cSwUploadRing::Kind::ClearAll;
  uploadRing_.publish();
  return true;
}

void cSoftwareRenderer::enqueuePbrMaterials(const cMaterialReg& reg) {
  if (!uploadRingEnabled_) { setPbrMaterials(reg); return; }
  // The table is announced, not copied: setPbrMaterials stores the pointer
  // and the render side reads the registry live through it, so announcing
  // the same table twice is a no-op and content edits under a stable
  // pointer stay visible exactly as the per-frame direct call left them.
  // The executor applies the announcement at the next seal, ahead of that
  // seal's meshes — the game's update order (materials, then uploads).
  if (ringMaterialsEnqueued_ == &reg) return;
  ringMaterialsEnqueued_ = &reg;
  pendingPbrMaterials_.store(&reg, std::memory_order_release);
}

cSoftwareRenderer::UploadRingStats cSoftwareRenderer::uploadRingStats() const {
  UploadRingStats st;
  st.pushed = uploadRing_.pushedCount();
  st.pushFull = uploadRing_.pushFullCount();
  st.maxOccupancy = uploadRing_.maxOccupancy();
  st.drained = uploadRing_.drainedTotal();
  st.applied = ringApplied_;
  st.coalesced = ringCoalesced_;
  st.clears = ringClears_;
  return st;
}

// ---- Design B Stage 2: the upload ring (consumer side — the seal) --------
// Runs on the detached executor inside renderFrameDetached, before the
// frame's renderFrame. The store's mutation discipline is unchanged: the
// survivors apply through the same three entry points (each bumps
// storeGen_), in ring order, and nothing else on the split path mutates
// the store — main's uploads all arrive here.
void cSoftwareRenderer::drainUploadRing(uint64_t target) {
  if (!uploadRingEnabled_) return;
  // The announced PBR material table lands first (the game's update order:
  // setPbrMaterials precedes uploadMeshes), ahead of this seal's meshes.
  if (const cMaterialReg* reg = pendingPbrMaterials_.exchange(nullptr, std::memory_order_acq_rel))
    setPbrMaterials(*reg);
  drainBatch_.clear();  // normally already retired after the last render; a throwing drain leaves it for here
  const size_t n = uploadRing_.drainUpTo(target, drainBatch_);
  ringDrained_ += n;
  if (n == 0) return;
  // Consumer-side coalescing (Argon trap 2): within one drain, only the
  // LAST entry per (kind, store key) can matter, and everything before the
  // last ClearAll is wiped by it. Superseded entries are skipped whole —
  // their face-bucket/emitter work never runs. A tombstone is an ordinary
  // last-entry: it survives unless a NEWER entry for its own key follows
  // it in the same drain (which defines the final state anyway) — it is
  // never dropped in favour of an older entry, and backpressure upstream
  // never drops it at all (must-push + the game's spill queue).
  size_t begin = 0;
  for (size_t i = 0; i < n; ++i)
    if (drainBatch_[i].kind == cSwUploadRing::Kind::ClearAll) begin = i;  // the last clear starts the kept range
  std::unordered_map<std::pair<int64_t, int>, size_t, PairHash> lastOf;
  for (size_t i = begin; i < n; ++i) {
    const cSwUploadRing::Slot& s = drainBatch_[i];
    if (s.kind == cSwUploadRing::Kind::ClearAll) continue;  // the clear itself always applies
    const int64_t key = s.kind == cSwUploadRing::Kind::Chunk
                            ? swMeshKey(s.mesh.cx, s.mesh.sy, s.mesh.cz)
                            : swMeshKey(s.pbrMesh.cx, s.pbrMesh.sy, s.pbrMesh.cz);
    lastOf[{key, int(s.kind)}] = i;
  }
  size_t appliedNow = 0;
  for (size_t i = begin; i < n; ++i) {
    const cSwUploadRing::Slot& s = drainBatch_[i];
    switch (s.kind) {
      case cSwUploadRing::Kind::ClearAll:
        clearChunkMeshes();
        ++ringClears_;
        ++appliedNow;
        break;
      case cSwUploadRing::Kind::Chunk:
        if (lastOf[{swMeshKey(s.mesh.cx, s.mesh.sy, s.mesh.cz), int(s.kind)}] != i) break;  // superseded
        uploadChunkMesh(s.mesh);
        ++appliedNow;
        break;
      case cSwUploadRing::Kind::PbrChunk:
        if (lastOf[{swMeshKey(s.pbrMesh.cx, s.pbrMesh.sy, s.pbrMesh.cz), int(s.kind)}] != i) break;  // superseded
        uploadPbrChunkMesh(s.pbrMesh);
        ++appliedNow;
        break;
    }
  }
  ringApplied_ += appliedNow;
  ringCoalesced_ += n - appliedNow;  // pre-clear wipe + superseded: drained but never applied
}
void cSoftwareRenderer::setDetachedPoolMode(bool on) { cSwBandPool::get().setDetachedMode(on); }

void cSoftwareRenderer::clear(uint32_t color) {
  // Cortana 2026-10-15 1455: runtime-dispatched fill (AVX2 or baseline; identical bytes).
  swsimd::fill32(fb_.pixels.data(), fb_.pixels.size(), color);
  swsimd::fillF(depth_.data(), depth_.size(), 1.0f);
  hizReset();  // Cortana 2026-10-15 1715: Hi-Z
}

// Cortana 2026-10-15 1715: hierarchical-Z [ENGINE] (see software_renderer.h).
void cSoftwareRenderer::hizReset() {
  hizFbW_ = fb_.width;
  hizFbH_ = fb_.height;
  hizTilesX_ = (fb_.width + kHizTile - 1) / kHizTile;
  const size_t n = size_t(hizTilesX_) * ((fb_.height + kHizTile - 1) / kHizTile);
  hizMax_.assign(n, std::numeric_limits<float>::infinity());
  hizDirty_.assign(n, uint8_t(0));
  if (depth_.size() != size_t(fb_.width) * fb_.height) hizFbW_ = 0;  // unpaired: stay off
}

float cSoftwareRenderer::hizTileMax(int tx, int ty) {
  const size_t i = size_t(ty) * hizTilesX_ + size_t(tx);
  if (hizDirty_[i]) {
    hizDirty_[i] = 0;
    const int xa = tx * kHizTile, ya = ty * kHizTile;
    const int xb = std::min(int(fb_.width), xa + kHizTile), yb = std::min(int(fb_.height), ya + kHizTile);
    float m = -std::numeric_limits<float>::infinity();
    for (int y = ya; y < yb; ++y) {
      const float* row = depth_.data() + size_t(y) * fb_.width;
      for (int x = xa; x < xb; ++x) m = std::max(m, row[x]);
    }
    hizMax_[i] = m;
  }
  return hizMax_[i];
}

// Transform a PackedVertex position to screen space.
// PackedVertex x,y,z are in 1/16 block units local to sub-chunk.
static void transformVertex(const PackedVertex& v, const float* mvp,
                            float* out, uint32_t width, uint32_t height) {
  // Position in block units (1/16 -> 1.0)
  float x = v.x / 16.0f, y = v.y / 16.0f, z = v.z / 16.0f;
  // MVP transform (column-major 4x4)
  float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z + mvp[12];
  float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z + mvp[13];
  float cz = mvp[2]*x + mvp[6]*y + mvp[10]*z + mvp[14];
  float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
  // Preserve the sign: callers reject behind-camera geometry via w <= 0, so
  // clamping a negative w to +1e-6f would defeat near-plane rejection.
  if (std::abs(cw) < 1e-6f) cw = std::copysign(1e-6f, cw);
  // Perspective divide + viewport. out[3] carries the clip-space w so callers
  // can reject behind-camera geometry (w <= 0 projects mirrored; fix 2026-10-11).
  const float invW = 1.0f / cw;
  out[0] = (cx * invW * 0.5f + 0.5f) * width;
  out[1] = (1.0f - (cy * invW * 0.5f + 0.5f)) * height;  // flip Y
  out[2] = cz * invW;  // depth 0..1
  out[3] = cw;
}

// Backface culling: skip if triangle faces away.
// Screen space is y-DOWN (transformVertex flips Y), which is orientation-reversing:
// a triangle wound CCW seen from the face normal (the mesher/GL convention, kept by
// glFrontFace(GL_CCW)) lands CLOCKWISE on screen, i.e. with negative screen-space
// cross. So on screen, CCW (cross >= 0) = backface. (Fix 2026-10-11: this was
// `cross <= 0`, culling every front face.)
static bool isBackface(const float* v0, const float* v1, const float* v2) {
  float cross = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
  return cross >= 0;  // CCW on the y-down screen = backface
}

// Model dithering (JD 2026-10-13): per-triangle classification against the
// fade band. A fragment's view depth is the perspective-correct interpolation
// of the vertices' clip w, which lies within [min w, max w] of the triangle,
// so: 1 = every fragment at/past far (keep all, zero per-pixel cost),
// 0 = every fragment at/before near (drop the whole triangle — fully faded
// geometry is not rasterized at all), 2 = straddling (per-pixel test).
static int ditherTriClass(bool on, float near, float far, const float* v0,
                          const float* v1, const float* v2) {
  if (!on) return 1;
  const float mn = std::min({v0[3], v1[3], v2[3]});
  const float mx = std::max({v0[3], v1[3], v2[3]});
  if (mn >= far) return 1;
  if (mx <= near) return 0;
  return 2;
}

void cSoftwareRenderer::rasterizeTriangle(const float* v0, const float* v1,
                                         const float* v2, uint32_t color) {
  if (isBackface(v0, v1, v2)) return;  // cull
  const int ditherCls = ditherTriClass(ditherEnabled_, ditherNear_, ditherFar_, v0, v1, v2);
  if (ditherCls == 0) return;  // fully faded: nothing to draw, no depth write

  // Bounding box (clamped)
  int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
  int x1 = std::min(int(fb_.width)-1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
  int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
  int y1 = std::min(int(fb_.height)-1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));
  y0 = std::max(y0, tlBandY0);  // RND-H3 band
  y1 = std::min(y1, tlBandY1 - 1);
  x0 = std::max(x0, tlBandX0); x1 = std::min(x1, tlBandX1 - 1);  // step 1 x tile
  if (y0 > y1 || x0 > x1) return;

  // Edge function setup
  auto edgeFn = [](const float* a, const float* b, float x, float y) {
    return (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0]);
  };
  const float area = edgeFn(v0, v1, v2[0], v2[1]);
  if (std::abs(area) < 1e-9f) return;

  // RND-H3 (2026-10-15): ONE divide per triangle (invArea) and incremental
  // edge functions: each barycentric is affine in x, so stepping one pixel
  // adds a constant. Replaces three edge evaluations + three divides per
  // pixel. Same coverage rule (w >= 0) and depth interpolation as before.
  const float invArea = 1.0f / area;
  const float dw0dx = -(v2[1] - v1[1]) * invArea, dw1dx = -(v0[1] - v2[1]) * invArea, dw2dx = -(v1[1] - v0[1]) * invArea;
  const float dzdx = dw0dx * v0[2] + dw1dx * v1[2] + dw2dx * v2[2];
  for (int y = y0; y <= y1; ++y) {
    const float px0 = x0 + 0.5f, py = y + 0.5f;
    float w0 = edgeFn(v1, v2, px0, py) * invArea;
    float w1 = edgeFn(v2, v0, px0, py) * invArea;
    float w2 = edgeFn(v0, v1, px0, py) * invArea;
    float depth = w0*v0[2] + w1*v1[2] + w2*v2[2];
    size_t idx = size_t(y) * fb_.width + size_t(x0);
    for (int x = x0; x <= x1; ++x, ++idx, w0 += dw0dx, w1 += dw1dx, w2 += dw2dx, depth += dzdx) {
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      if (!(depth < depth_[idx])) continue;
      if (ditherCls == 2) {
        const float viewDepth = 1.0f / (w0 / v0[3] + w1 / v1[3] + w2 / v2[3]);
        if (!modelDitherKeep(x, y, viewDepth, ditherNear_, ditherFar_)) continue;
      }
      depth_[idx] = depth;
      fb_.pixels[idx] = color;
    }
  }
}

// Face colors: flat shading by face direction (top brightest, bottom darkest).
//
// FLAT-MODE MODE MAP (P3-3, Appendix D L3 — exact, do not over-read):
// the flat path collapses PackedVertex shading modes onto two cases:
//   - kShadeStripTint0 / kShadeStripTint1 (modes 3 / 4): a reddish tint of
//     the value's luminance (parity detail from the shader is NOT
//     reproduced);
//   - EVERY other mode (kShadeTex, kShadePalette16/32/64, kShadePips,
//     kShadeCells, kShadeAnimated, kShadeGlass, ...): plain grayscale
//     from `value` — palette indices, pip/cell counts and animation
//     indices are NOT resolved to their shader colors here.
// Consequence: flat-mode consumers (MCP-vision captures are flat by
// design) see strip-tinted or grayscale geometry, never the palette /
// mask colors the per-pixel path (or GL) shows. That is the documented
// simplification of this path, not a bug — read flat captures as
// geometry/luminance, not as color truth.
static uint32_t faceColor(uint8_t face, uint8_t value, uint8_t mode) {
  // Base color from mode/value (simplified: use value as brightness)
  uint8_t b = value;
  // Face shading: top=1.0, sides=0.7, bottom=0.4
  float shade = 0.7f;
  if (face == 2) shade = 1.0f;      // top
  else if (face == 3) shade = 0.4f; // bottom
  uint8_t v = uint8_t(b * shade);
  // Trace colors: mode determines red/blue tint
  if (mode == 3) return 0xFF000000 | (v << 16) | (v >> 1);  // kShadeStripTint0: reddish
  if (mode == 4) return 0xFF000000 | (v >> 2 << 16) | (v >> 1 << 8) | v;  // 2026-10-24: kShadeStripTint1 bluish (was reddish)
  return 0xFF000000 | (v << 16) | (v << 8) | v;  // grayscale
}

// Cortana 2026-10-15 1350: Trace power colour (CloudAtlas SHADER_NOTE r3):
// lum = mask.r / 0.70; lum <= 1: rgb = ramp[s] * lum; else mix toward white
// by min(1, (lum-1)*2.5) * (s/64)^2. Lighting = shaded / unshaded mask ratio
// so the tinted face keeps the basic shader's N.L shading.
static uint32_t traceTint(uint32_t texel, uint32_t shaded, bool blue, uint8_t value) {
  const int s = value > 64 ? 64 : value;
  // 2026-10-24 (JD): unpowered = dark channel hue (ramp[1] x 0.45), not the ramp's neutral grey;
  // mirrors chunk_shading.glsl MODE_WIRE.
  const uint32_t r1 = (blue ? kBlueTraceRamp : kRedTraceRamp)[1];
  const uint32_t rc = s ? (blue ? kBlueTraceRamp : kRedTraceRamp)[s]
                        : (((((r1 >> 16) & 0xFFu) * 45u / 100u) << 16) | ((((r1 >> 8) & 0xFFu) * 45u / 100u) << 8) | ((r1 & 0xFFu) * 45u / 100u));
  const float mr = float((texel >> 16) & 0xFF);
  const float light = mr > 0.f ? std::min(1.f, float((shaded >> 16) & 0xFF) / mr) : 1.f;
  const float lum = mr / 255.f / 0.70f;
  float c[3] = {float((rc >> 16) & 0xFF), float((rc >> 8) & 0xFF), float(rc & 0xFF)};
  if (lum <= 1.f) { for (float& ch : c) ch *= lum; }
  else {
    const float k = std::min(1.f, (lum - 1.f) * 2.5f) * (float(s) / 64.f) * (float(s) / 64.f);
    for (float& ch : c) ch = ch + (255.f - ch) * k;
  }
  auto q = [light](float v) { const float o = v * light; return uint32_t(o > 255.f ? 255.f : o); };
  return 0xFF000000u | (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
}

/// Per-pixel rasterization with shader (slower, higher quality).
/// `groundFx` marks a kShadeGroundPulse quad of the mesh renderFrame is
/// currently drawing: the wafer ground pulse + ground traces emissives
/// (CPU mirrors) add onto the shaded color, mirroring chunk_shading.glsl.
// Ginny (Argon) 2026-10-16 Pass 2 step 1: native GreedyQuad setup. rasterizeTrisShaded
// rasterizes 1 or 2 triangles (a quad split v0v1v2 + v0v2v3) with ONE shared setup (fragment
// input, shader classification, Hi-Z state) and ONE row walk over the union of their rows.
// Per row it runs triangle A's span then triangle B's span, so every pixel sees the same
// sequence of depth tests and writes as two separate rasterizeTriangleShaded calls: output is
// byte-identical (Hi-Z row skips stay exact per triangle). LL_SW_P2 bit 0 (default on).
void cSoftwareRenderer::rasterizeTriangleShaded(const float* v0, const float* v1,
                                              const float* v2, const ShaderVaryings* s0,
                                              const ShaderVaryings* s1, const ShaderVaryings* s2,
                                              FragmentShaderFn shader, bool groundFx) {
  const float* tv[2][3] = {{v0, v1, v2}, {nullptr, nullptr, nullptr}};
  const ShaderVaryings* ts[2][3] = {{s0, s1, s2}, {nullptr, nullptr, nullptr}};
  rasterizeTrisShaded(1, tv, ts, shader, groundFx);
}

namespace {
struct SwTriSetup {
  const float *v0, *v1, *v2;
  const ShaderVaryings *s0, *s1, *s2;
  int ditherCls, x0, x1, y0, y1, htx0, htx1, skipTo;
  float invArea, dw0dx, dw1dx, dw2dx, dzdx, iw0, iw1, iw2, triMinZ, hizMargin;
  int mip = 0;  // Cortana 2026-10-16 (BUG-0005): per-triangle texture LOD
  bool hasEl = false;  // Cortana 2026-10-16 (render.emissive_light)
  float el[3][3];      // per-vertex emitted light (rgb)
  bool simdSpan;
  swsimd::ShadeSpanBasic span;
};
}  // namespace

void cSoftwareRenderer::rasterizeTrisShaded(int nt, const float* const (*tv)[3],
                                           const ShaderVaryings* const (*ts)[3],
                                           FragmentShaderFn shader, bool groundFx) {
  auto edgeFn = [](const float* a, const float* b, float x, float y) {
    return (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0]);
  };
  FragmentInput fin{};  // value-init: unset map pointers are null, not indeterminate
  bool finReady = false;
  // Cortana 2026-10-16 (BUG-0005): per-triangle mip level -> fin texel addressing.
  int curMip = 0;
  const uint32_t* baseTex = nullptr;
  const int baseTile = textures_.tileSize();
  auto mipShift = [](uint32_t ts) { return ts >= 64 ? 6u : ts >= 32 ? 5u : ts >= 16 ? 4u : ts >= 8 ? 3u : 2u; };
  auto setFinMip = [&](int m) {
    if (m == curMip || !finReady) return;
    curMip = m;
    const uint32_t ts = uint32_t(baseTile) >> m;
    fin.texShift = mipShift(ts); fin.texMask = ts - 1; fin.texArea = ts * ts; fin.texScale = float(ts - 1);
    fin.texData = m == 0 ? baseTex : textures_.mipData(m);
  };
  // 2026-10-16 (JD bug 1): colour lamp 16/32/64, the CPU mirror of chunk_shading.glsl's
  // vMode 5-7 branch (palette colour (s-1) mod N inside the tier mask, highlight, rim) on the
  // 16x16 tiles; lighting = the basic shader's ratio, then the GL emissive mix.
  auto lampShade = [this](const FragmentInput& f, uint32_t shaded) -> uint32_t {
    const int tier = tlQuadMode - kShadePalette16, n = 16 << tier;
    const int sv = std::clamp(int(tlQuadValue), 1, 64);
    const uint32_t* row = textures_.paletteRow(lampPal_[size_t(tier)]);
    if (!row || !f.texData || f.varyings.layer >= f.texLayers) return shaded;
    const uint32_t tx = uint32_t(swTile(f.varyings.u) * f.texScale) & f.texMask, ty = uint32_t(swTile(f.varyings.v) * f.texScale) & f.texMask;
    const uint32_t ti = swTexelIndex(f, tx, ty);
    auto rgb = [](uint32_t c, float o[3]) { o[0] = float((c >> 16) & 0xFF) / 255.f; o[1] = float((c >> 8) & 0xFF) / 255.f; o[2] = float(c & 0xFF) / 255.f; };
    auto layerTexel = [&](int layer) -> uint32_t {
      return layer >= 0 && uint32_t(layer) < f.texLayers ? f.texData[size_t(layer) * f.texArea + ti] : 0u;
    };
    float t[3], m[3], c[3], rim[3];
    rgb(f.texData[size_t(f.varyings.layer) * f.texArea + ti], t);
    rgb(layerTexel(lampMask_[size_t(tier)]), m);
    rgb(row[(sv - 1) % n], c);
    rgb(layerTexel(lampRim_[size_t(tier)]), rim);
    const float lum = std::max({t[0], t[1], t[2]});
    const float hi = 0.30f * std::clamp((lum - 0.78f) / 0.22f, 0.f, 1.f), sc = std::min(1.f, lum / 0.784f);
    const float luma = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
    const float rimK = 0.60f * std::pow(1.f - luma, 10.f) * rim[0];
    static constexpr float kRim[3] = {185.f / 255.f, 168.f / 255.f, 240.f / 255.f};
    const float emissive = 0.8f * m[0] + 0.1f;
    // the basic shader's light factor, from its shaded output vs the raw texel
    const float tl = std::max({t[0], t[1], t[2]});
    const float sl = std::max({float((shaded >> 16) & 0xFF), float((shaded >> 8) & 0xFF), float(shaded & 0xFF)}) / 255.f;
    const float light = tl > 0.f ? std::min(1.f, sl / tl) : 1.f;
    uint32_t out = 0xFF000000u;
    for (int k = 0; k < 3; ++k) {
      float col = (c[k] + (1.f - c[k]) * hi) * sc;
      col = col + (1.f - col) * 0.55f * m[1];
      float o = t[k] + (col - t[k]) * m[0];
      o = o + (c[k] - o) * 0.35f * m[2];
      o = o + (kRim[k] - o) * rimK;
      const float lit = o * light;
      const float v = lit + (o - lit) * emissive;
      out |= uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f) << (16 - 8 * k);
    }
    return out;
  };
  // 2026-10-16 crystal twinkle: CPU mirror of chunk_shading.glsl's MODE_LEAF block, integer
  // only. Glow R = strength, G = phase; k = R * LUT[(phase + G) & 255] / 255, lerp to white.
  auto leafTwinkle = [this](const FragmentInput& f, uint32_t c) -> uint32_t {
    const int gl = leafGlow_[size_t(std::min<int>(tlQuadValue, 4) - 1)];
    if (gl <= 0 || uint32_t(gl) >= f.texLayers) return c;
    const uint32_t tx = uint32_t(swTile(f.varyings.u) * f.texScale) & f.texMask, ty = uint32_t(swTile(f.varyings.v) * f.texScale) & f.texMask;
    const uint32_t g = f.texData[size_t(gl) * f.texArea + swTexelIndex(f, tx, ty)];
    const uint32_t R = (g >> 16) & 0xFF, G = (g >> 8) & 0xFF;
    if (!R) return c;
    // Travelling wave across the canopy (mirror of the GLSL sweep, in 1/256 turns).
    const float wx = f.varyings.wx + meshOriginX_, wy = f.varyings.wy + meshOriginY_, wz = f.varyings.wz + meshOriginZ_;
    const int32_t sweep = int32_t(std::floor((wx * 0.11f + wy * 0.05f + wz * 0.07f) * 256.f));
    const uint32_t ph = uint32_t(int32_t(twinklePhase_) - sweep + int32_t(G * 90u / 256u));
    const uint32_t k = R * kTwinkleLut[ph & 255u] / 255u;  // 0..204
    uint32_t out = c & 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
      const uint32_t ch = (c >> sh) & 0xFF;
      out |= (ch + ((255u - ch) * k + 127u) / 255u) << sh;
    }
    return out;
  };
  // 2026-10-16 (Wafer beta art): kShadeTraceTint, CPU mirror of chunk_shading.glsl MODE_TRACE_TINT:
  // base = mix(texel, Trace ramp colour (end when powered, start when not), mask R); lit by the
  // basic shader's light ratio, emissive 0.85 * mask when powered.
  auto traceTintShade = [this](const FragmentInput& f, uint32_t shaded) -> uint32_t {
    const int v = tlQuadValue;
    const int maskLayer = tintMask_[size_t(v & 31)];
    const uint32_t* row = textures_.paletteRow(stripPal_[size_t((v >> 6) & 1)]);
    if (!row || maskLayer <= 0 || uint32_t(maskLayer) >= f.texLayers || !f.texData || f.varyings.layer >= f.texLayers) return shaded;
    const bool on = (v & 128) != 0;
    if (!on) return shaded;  // 2026-10-16 (Discovered by J.D.): grey until powered, matching the shader
    const uint32_t tx = uint32_t(swTile(f.varyings.u) * f.texScale) & f.texMask, ty = uint32_t(swTile(f.varyings.v) * f.texScale) & f.texMask;
    const uint32_t ti = swTexelIndex(f, tx, ty);
    const uint32_t t = f.texData[size_t(f.varyings.layer) * f.texArea + ti];
    const float m = float((f.texData[size_t(maskLayer) * f.texArea + ti] >> 16) & 0xFF) / 255.f;
    const uint32_t c = row[on ? 64 : 0];
    const float tl = float(std::max({(t >> 16) & 0xFFu, (t >> 8) & 0xFFu, t & 0xFFu})) / 255.f;
    const float sl = float(std::max({(shaded >> 16) & 0xFFu, (shaded >> 8) & 0xFFu, shaded & 0xFFu})) / 255.f;
    const float light = tl > 0.f ? std::min(1.f, sl / tl) : 1.f;
    const float em = on ? 0.85f * m : 0.f;
    uint32_t out = 0xFF000000u;
    for (int sh = 16; sh >= 0; sh -= 8) {
      const float b = float((t >> sh) & 0xFF) / 255.f, tc = float((c >> sh) & 0xFF) / 255.f;
      const float o = b + (tc - b) * m, lit = o * light, val = lit + (o - lit) * em;
      out |= uint32_t(std::clamp(val, 0.f, 1.f) * 255.f + 0.5f) << sh;
    }
    return out;
  };
  const bool fx = groundFx && groundFx_.active();
  // Cortana 2026-10-15 1320: the pulse emissive is a pure function of the
  // fragment's CHUNK (sin + pow per pixel before); memoize the last chunk.
  int memoCx = INT32_MIN, memoCz = INT32_MIN;
  std::array<float, 3> memoE{0.f, 0.f, 0.f};
  const bool hiz = hizFbW_ == fb_.width && hizFbH_ == fb_.height && !hizMax_.empty();
  SwTriSetup T[2];
  int nLive = 0, Y0 = INT32_MAX, Y1 = -1;
  const ShaderVaryings* memoElV[6]; float memoEl[6][4]; int nMemoEl = 0;  // kP2EmitMemo (per call = per quad)
  for (int t = 0; t < nt; ++t) {
    const float *v0 = tv[t][0], *v1 = tv[t][1], *v2 = tv[t][2];
    const ShaderVaryings *s0 = ts[t][0], *s1 = ts[t][1], *s2 = ts[t][2];
    if (isBackface(v0, v1, v2)) continue;
    const int ditherCls = ditherTriClass(ditherEnabled_, ditherNear_, ditherFar_, v0, v1, v2);
    if (ditherCls == 0) continue;  // fully faded (JD 2026-10-13): skip shading entirely
    int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
    int x1 = std::min(int(fb_.width)-1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
    int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
    int y1 = std::min(int(fb_.height)-1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));
    y0 = std::max(y0, tlBandY0);  // RND-H3 band
    y1 = std::min(y1, tlBandY1 - 1);
    x0 = std::max(x0, tlBandX0); x1 = std::min(x1, tlBandX1 - 1);  // step 1 x tile
    if (y0 > y1 || x0 > x1) continue;
    if (swP2(kP2CentreCull)) {  // Cortana 2026-10-25 box (Archie step 3): no pixel centre in the bbox -> no sample can pass
      constexpr float kEps = 1e-3f;  // conservative vs edge-function rounding at the bbox edge
      const int cx0 = std::max(x0, int(std::ceil(std::min({v0[0], v1[0], v2[0]}) - 0.5f - kEps)));
      const int cx1 = std::min(x1, int(std::floor(std::max({v0[0], v1[0], v2[0]}) - 0.5f + kEps)));
      const int cy0 = std::max(y0, int(std::ceil(std::min({v0[1], v1[1], v2[1]}) - 0.5f - kEps)));
      const int cy1 = std::min(y1, int(std::floor(std::max({v0[1], v1[1], v2[1]}) - 0.5f + kEps)));
      if (cx0 > cx1 || cy0 > cy1) { if (g_swProfOn) g_centreCulled.fetch_add(1, std::memory_order_relaxed); continue; }
    }
    const float area = edgeFn(v0, v1, v2[0], v2[1]);
    if (std::abs(area) < 1e-9f) continue;
    if (!finReady) {
      fin.texData = textures_.data(); fin.texLayers = textures_.layers();
      {  // Cortana 2026-10-16 (BUG-0003): the colour array's runtime tile size
        const uint32_t tileSz = uint32_t(textures_.tileSize());
        fin.texShift = tileSz == 64 ? 6u : tileSz == 32 ? 5u : 4u;
        fin.texMask = tileSz - 1; fin.texArea = tileSz * tileSz; fin.texScale = float(tileSz - 1);
      }
      if (swP2(kP2Morton) && textures_.dataMorton()) { fin.texData = textures_.dataMorton(); fin.texMorton = true; }  // Pass 2 step 4 (16x16 only)
      fin.pbr = effectivePbrContext();
      finReady = true;
      baseTex = fin.texData; curMip = 0;
    }
    SwTriSetup& S = T[nLive++];
    S.mip = 0;
    if (mipOn_ && textures_.mipLevels() > 1) {  // Cortana 2026-10-16 (BUG-0005): texel/pixel ratio LOD
      const float du1 = s1->u - s0->u, dv1 = s1->v - s0->v, du2 = s2->u - s0->u, dv2 = s2->v - s0->v;
      const float uvA = std::fabs(du1 * dv2 - du2 * dv1) * float(baseTile) * float(baseTile);
      const float ratio = uvA / std::max(std::fabs(area), 1e-6f);
      if (ratio > 1.0f) S.mip = std::min(textures_.mipLevels() - 1, int(std::floor(0.5f * std::log2(ratio) + 0.5f)));
    }
    S.v0 = v0; S.v1 = v1; S.v2 = v2; S.s0 = s0; S.s1 = s1; S.s2 = s2;
    S.ditherCls = ditherCls; S.x0 = x0; S.x1 = x1; S.y0 = y0; S.y1 = y1; S.skipTo = y0;
    // RND-H3 (2026-10-15): one divide per triangle + incremental edges, and
    // PERSPECTIVE-CORRECT varyings (one reciprocal per SHADED pixel).
    S.invArea = 1.0f / area;
    S.dw0dx = -(v2[1] - v1[1]) * S.invArea; S.dw1dx = -(v0[1] - v2[1]) * S.invArea; S.dw2dx = -(v1[1] - v0[1]) * S.invArea;
    S.dzdx = S.dw0dx * v0[2] + S.dw1dx * v1[2] + S.dw2dx * v2[2];
    S.iw0 = 1.0f / v0[3]; S.iw1 = 1.0f / v1[3]; S.iw2 = 1.0f / v2[3];
    // Cortana 2026-10-15 1715: hierarchical-Z early reject [ENGINE] (exact: rows
    // restart from exact edge functions, so drawn rows produce the same bits).
    S.triMinZ = std::min({v0[2], v1[2], v2[2]});
    S.hizMargin = 1e-4f * std::max(1.0f, std::fabs(S.triMinZ));
    S.htx0 = x0 / kHizTile; S.htx1 = x1 / kHizTile;
    // Ginny 2026-10-16 (perf list, Cortana): SIMD span shading for the common case (opt-in
    // LL_SW_SIMD_SHADE=1; bit-identical to the scalar loop; see swsimd::shadeSpanBasic).
    bool basicShade = shader == &basicFragmentShader;
    if (!basicShade && shader == &pbrFragmentShader) {
      const PbrFragmentContext* pc = fin.pbr;
      basicShade = !((pc && pc->registry) ? pc->registry->defFor(s0->materialId) : nullptr);
    }
    // Pass 2 step 5: with LL_SW_P2 bit kP2Dispatch cleared the span stays scalar (A/B proofs).
    S.simdSpan = basicShade && swsimd::shadeSpanEnabled() && swP2(kP2Dispatch) && !fx && ditherCls != 2 &&
                 tlQuadMode != kShadeStripTint0 && tlQuadMode != kShadeStripTint1 &&
                 tlQuadMode != kShadeCutout && tlQuadMode != kShadeAlphaBlend && tlQuadMode != kShadeLeaf && tlQuadMode != kShadeTraceTint &&
                 (tlQuadMode < kShadePalette16 || tlQuadMode > kShadePalette64) &&
                 fin.texData && s0->layer < fin.texLayers;
    // Argon point 3 (2026-10-18): AO tris no longer excluded — the span kernels
    // interpolate AO with the scalar loop's exact op order and apply its
    // post-shader channel scale, so AO shading is bit-identical in-kernel.
    S.hasEl = false;
    if (!frameEmit_.empty()) {  // Cortana 2026-10-16 (render.emissive_light): per-vertex point lights
      constexpr float kR = 6.0f, kStr = 0.9f;
      const ShaderVaryings* sv3e[3] = {s0, s1, s2};
      const bool memoOn = swP2(kP2EmitMemo);
      for (int k = 0; k < 3; ++k) {
        // Cortana 2026-10-25 box (Archie step 1): a quad's two triangles share v0/v2, so
        // reuse the light already summed for that vertex in this call (same ops -> same bits).
        if (memoOn) {
          int hit = -1;
          for (int m = 0; m < nMemoEl; ++m) if (memoElV[m] == sv3e[k]) { hit = m; break; }
          if (hit >= 0) {
            S.el[k][0] = memoEl[hit][0]; S.el[k][1] = memoEl[hit][1]; S.el[k][2] = memoEl[hit][2];
            if (memoEl[hit][3] > 0.01f) S.hasEl = true;
            if (g_swProfOn) g_emitMemoHits.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
        }
        const float wx = sv3e[k]->wx + meshOriginX_, wy = sv3e[k]->wy + meshOriginY_, wz = sv3e[k]->wz + meshOriginZ_;
        float lr = 0, lg = 0, lb = 0;
        for (size_t i = 0; i + 5 < frameEmit_.size(); i += 6) {
          const float dx = frameEmit_[i] - wx, dy = frameEmit_[i + 1] - wy, dz = frameEmit_[i + 2] - wz;
          const float d2 = dx * dx + dy * dy + dz * dz;
          if (d2 >= kR * kR) continue;
          const float f = 1.0f - sqrtFast(d2) / kR, a = kStr * f * f;  // Carmack sqrt (per-pixel falloff)
          lr += frameEmit_[i + 3] * a; lg += frameEmit_[i + 4] * a; lb += frameEmit_[i + 5] * a;
        }
        S.el[k][0] = std::min(lr, 1.5f); S.el[k][1] = std::min(lg, 1.5f); S.el[k][2] = std::min(lb, 1.5f);
        if (lr + lg + lb > 0.01f) S.hasEl = true;
        if (memoOn && nMemoEl < 6) { memoElV[nMemoEl] = sv3e[k]; memoEl[nMemoEl][0] = S.el[k][0]; memoEl[nMemoEl][1] = S.el[k][1]; memoEl[nMemoEl][2] = S.el[k][2]; memoEl[nMemoEl][3] = lr + lg + lb; ++nMemoEl; }
      }
      if (S.hasEl) S.simdSpan = false;  // lit triangles shade scalar
    }
    S.span = swsimd::ShadeSpanBasic{};
    if (S.simdSpan) {
      swsimd::ShadeSpanBasic& span = S.span;
      span.dw0dx = S.dw0dx; span.dw1dx = S.dw1dx; span.dw2dx = S.dw2dx; span.dzdx = S.dzdx;
      span.iw0 = S.iw0; span.iw1 = S.iw1; span.iw2 = S.iw2;
      const ShaderVaryings* sv3[3] = {s0, s1, s2};
      for (int k = 0; k < 3; ++k) {
        span.a[k][0] = sv3[k]->nx; span.a[k][1] = sv3[k]->ny; span.a[k][2] = sv3[k]->nz;
        span.a[k][3] = sv3[k]->u; span.a[k][4] = sv3[k]->v;
        span.ao[k] = sv3[k]->ao;  // Argon point 3: per-vertex AO for the kernel
      }
      span.hasAo = s0->ao < 1.0f || s1->ao < 1.0f || s2->ao < 1.0f;  // == scalar loop's aoTri
      {  // Cortana 2026-10-16: runtime tile + per-triangle mip (BUG-0005)
        const uint32_t tileSz = uint32_t(baseTile) >> S.mip;
        span.tex = (S.mip ? textures_.mipData(S.mip) : fin.texData) + size_t(s0->layer) * size_t(tileSz * tileSz);
        span.texShift = int(mipShift(tileSz)); span.texScale = float(tileSz - 1);
      }
      span.morton = fin.texMorton;
    }
    Y0 = std::min(Y0, y0); Y1 = std::max(Y1, y1);
  }
  for (int y = Y0; y <= Y1; ++y) {
   for (int t = 0; t < nLive; ++t) {
    SwTriSetup& S = T[t];
    if (y < S.skipTo || y > S.y1) continue;
    setFinMip(S.mip);  // Cortana 2026-10-16 (BUG-0005)
    const float *v0 = S.v0, *v1 = S.v1, *v2 = S.v2;
    const ShaderVaryings *s0 = S.s0, *s1 = S.s1, *s2 = S.s2;
    const int ditherCls = S.ditherCls;
    const float invArea = S.invArea, dw0dx = S.dw0dx, dw1dx = S.dw1dx, dw2dx = S.dw2dx, dzdx = S.dzdx;
    const float iw0 = S.iw0, iw1 = S.iw1, iw2 = S.iw2;
    const bool simdSpan = S.simdSpan;
    swsimd::ShadeSpanBasic& span = S.span;
    if (g_swProfOn) ++g_rowsTotal;
    if (hiz && (y == S.y0 || y % kHizTile == 0)) {
      bool occluded = true;
      for (int tx = S.htx0; tx <= S.htx1 && occluded; ++tx) occluded = hizTileMax(tx, y / kHizTile) + S.hizMargin < S.triMinZ;
      if (occluded) {
        const int last = std::min(S.y1, y | (kHizTile - 1));
        if (g_swProfOn) { g_hizRows += uint64_t(last - y + 1); g_rowsTotal += uint64_t(last - y); }  // pass sw2: rows skipped past count in the total
        S.skipTo = last + 1;
        continue;
      }
    }
    int wxLo = INT32_MAX, wxHi = -1;  // Cortana 2026-10-15 1715: written x span (Hi-Z dirty)
    const float py = y + 0.5f;
    float px0 = S.x0 + 0.5f;
    float sw0 = edgeFn(v1, v2, px0, py) * invArea;
    float sw1 = edgeFn(v2, v0, px0, py) * invArea;
    float sw2 = edgeFn(v0, v1, px0, py) * invArea;
    // Cortana 2026-10-25 (box, Archie SW setup pass): kP2SpanClip solves each edge for the
    // x-range where it is >= 0 on this row and walks only that (1 px margin each side),
    // instead of the full bbox width. Opt-in: edge pixels may differ by float rounding.
    int x1 = S.x1;
    const int x0 = [&] {
      if (!swP2(kP2SpanClip)) return S.x0;
      float lo = float(S.x0), hi = float(S.x1);
      const float sw[3] = {sw0, sw1, sw2}, dw[3] = {dw0dx, dw1dx, dw2dx};
      for (int e = 0; e < 3; ++e) {
        if (dw[e] > 0.f) lo = std::max(lo, float(S.x0) - sw[e] / dw[e]);
        else if (dw[e] < 0.f) hi = std::min(hi, float(S.x0) + sw[e] / -dw[e]);
        else if (sw[e] < 0.f) { hi = lo - 2.f; }
      }
      const int a = std::max(S.x0, int(std::floor(lo)) - 1), b = std::min(S.x1, int(std::ceil(hi)) + 1);
      x1 = b;
      return a;
    }();
    if (x1 < x0) continue;
    if (x0 != S.x0) {
      px0 = x0 + 0.5f;
      sw0 = edgeFn(v1, v2, px0, py) * invArea;
      sw1 = edgeFn(v2, v0, px0, py) * invArea;
      sw2 = edgeFn(v0, v1, px0, py) * invArea;
    }
    float sdepth = sw0*v0[2] + sw1*v1[2] + sw2*v2[2];
    size_t sidx = size_t(y) * fb_.width + size_t(x0);
    uint64_t lc = 0, ls = 0;
    if (simdSpan && x1 >= x0) {
      span.sw0 = sw0; span.sw1 = sw1; span.sw2 = sw2; span.sdepth = sdepth;
      span.depth = depth_.data() + sidx; span.pixels = fb_.pixels.data() + sidx; span.n = x1 - x0 + 1;
      swsimd::shadeSpanBasic(span);
      lc = span.lc; ls = span.ls;
      if (span.wxHi >= 0) { wxLo = x0 + span.wxLo; wxHi = x0 + span.wxHi; }
    }
    for (int x = simdSpan ? x1 + 1 : x0; x <= x1; ++x, ++sidx, sw0 += dw0dx, sw1 += dw1dx, sw2 += dw2dx, sdepth += dzdx) {
      if (sw0 >= 0 && sw1 >= 0 && sw2 >= 0) {
        const float depth = sdepth;
        const size_t idx = sidx;
        ++lc;
        if (depth < depth_[idx]) {
          ++ls;
          const float pw0 = sw0 * iw0, pw1 = sw1 * iw1, pw2 = sw2 * iw2;
          const float viewDepth = 1.0f / (pw0 + pw1 + pw2);
          if (ditherCls == 2) {
            // Screen-door discard BEFORE shading: faded fragments cost no
            // fragment work and write no depth (true discard semantics).
            if (!modelDitherKeep(x, y, viewDepth, ditherNear_, ditherFar_)) continue;
          }
          const float w0 = pw0 * viewDepth, w1 = pw1 * viewDepth, w2 = pw2 * viewDepth;  // perspective-correct
          // Interpolate varyings
          fin.varyings.nx = w0*s0->nx + w1*s1->nx + w2*s2->nx;
          fin.varyings.ny = w0*s0->ny + w1*s1->ny + w2*s2->ny;
          fin.varyings.nz = w0*s0->nz + w1*s1->nz + w2*s2->nz;
          fin.varyings.u = w0*s0->u + w1*s1->u + w2*s2->u;
          fin.varyings.v = w0*s0->v + w1*s1->v + w2*s2->v;
          fin.varyings.layer = s0->layer;
          fin.varyings.materialId = s0->materialId;  // flat per quad, like layer
          fin.varyings.wx = w0*s0->wx + w1*s1->wx + w2*s2->wx;
          fin.varyings.wy = w0*s0->wy + w1*s1->wy + w2*s2->wy;
          fin.varyings.wz = w0*s0->wz + w1*s1->wz + w2*s2->wz;
          const bool aoTri = s0->ao < 1.0f || s1->ao < 1.0f || s2->ao < 1.0f;  // Cortana 2026-10-16
          // Cortana 2026-10-15 1350: alpha cutout (0.5) + Trace power tint.
          uint32_t texel = 0xFFFFFFFFu;
          const bool strip = tlQuadMode == kShadeStripTint0 || tlQuadMode == kShadeStripTint1;
          if ((strip || tlQuadMode == kShadeCutout || tlQuadMode == kShadeLeaf) && fin.texData && fin.varyings.layer < fin.texLayers) {
            const uint32_t tx = uint32_t(swTile(fin.varyings.u) * fin.texScale) & fin.texMask, ty = uint32_t(swTile(fin.varyings.v) * fin.texScale) & fin.texMask;  // Cortana 2026-10-15 1705: swTile
            texel = fin.texData[fin.varyings.layer * fin.texArea + swTexelIndex(fin, tx, ty)];
            if ((texel >> 24) < 128u) continue;  // discard: no colour, no depth write
          }
          uint32_t color = shader(fin);
          if (strip) color = traceTint(texel, color, tlQuadMode == kShadeStripTint1, tlQuadValue);
          else if (tlQuadMode >= kShadePalette16 && tlQuadMode <= kShadePalette64) color = lampShade(fin, color);
          else if (tlQuadMode == kShadeLeaf && twinkleMode_ > 0 && tlQuadValue > 0) color = leafTwinkle(fin, color);
          else if (tlQuadMode == kShadeTraceTint) color = traceTintShade(fin, color);
          // Cortana 2026-10-16 (render.shimmer): lithography shimmer on POWERED Trace - a slow
          // diagonal brightness wave on the render clock (LL_RENDER_TIME pins it). Render-only.
          if (shimmerOn_ && tlQuadValue > 0 && (strip || (tlQuadMode == kShadeTraceTint && ((tlQuadValue & 0x80) || (llP2PerfOff() & 8))))) {  // 1520: powered only
            const float ph = shimmerTime_ * 2.4f + (fin.varyings.wx + fin.varyings.wz + meshOriginX_ + meshOriginZ_) * 1.7f;
            const float k = 1.0f + 0.16f * std::sin(ph);
            auto sc = [k](uint32_t c) { const float o = float(c) * k; return o > 255.f ? 255u : uint32_t(o); };
            color = (color & 0xFF000000u) | (sc((color >> 16) & 0xFF) << 16) | (sc((color >> 8) & 0xFF) << 8) | sc(color & 0xFF);
          }
          if (S.hasEl) {  // Cortana 2026-10-16 (render.emissive_light): coloured local light, render-only
            float L[3];
            for (int k = 0; k < 3; ++k) L[k] = w0 * S.el[0][k] + w1 * S.el[1][k] + w2 * S.el[2][k];
            auto li = [](uint32_t c, float l) { const float o = float(c) * (1.0f + l) + 48.f * l; return o > 255.f ? 255u : uint32_t(o); };
            color = (color & 0xFF000000u) | (li((color >> 16) & 0xFF, L[0]) << 16) | (li((color >> 8) & 0xFF, L[1]) << 8) | li(color & 0xFF, L[2]);
          }
          if (aoTri) {  // Cortana 2026-10-16 (render.ao): perspective-correct corner AO
            const float a = w0 * s0->ao + w1 * s1->ao + w2 * s2->ao;
            auto sc = [a](uint32_t c) { return uint32_t(float(c) * a); };
            color = (color & 0xFF000000u) | (sc((color >> 16) & 0xFF) << 16) | (sc((color >> 8) & 0xFF) << 8) | sc(color & 0xFF);
          }
          if (fx) {
            // Wafer ground pulse + traces (P1-RENDER H1): additive emissive
            // at the fragment's world position, from the mesh's baked
            // params — the CPU mirrors of the GLSL in chunk_shading.glsl.
            const float wx = fin.varyings.wx + meshOriginX_;
            const float wz = fin.varyings.wz + meshOriginZ_;
            float er = 0.f, eg = 0.f, eb = 0.f;
            if (groundFx_.pulseOn && groundFx_.pulseP0[2] > 0.f) {
              const GroundPulseParams pp{groundFx_.pulseP0[0], groundFx_.pulseP0[1],
                                         groundFx_.pulseP0[2], groundFx_.pulseP1[0],
                                         groundFx_.pulseP1[1], groundFx_.pulseP1[2]};
              const int ccx = int(std::floor(wx / 16.f)), ccz = int(std::floor(wz / 16.f));
              if (ccx != memoCx || ccz != memoCz) {
                memoE = groundPulseEmissive(groundFx_.pulseTimeSec, ccx, ccz, pp);
                memoCx = ccx; memoCz = ccz;
              }
              const auto& e = memoE;
              er += e[0]; eg += e[1]; eb += e[2];
            }
            if (groundFx_.traceOn && groundFx_.traceP0[2] > 0.f && groundFx_.traceP0[0] > 0.f) {
              const GroundTraceParams tp{groundFx_.traceP0[0], groundFx_.traceP0[1],
                                         groundFx_.traceP0[2], groundFx_.traceP0[3],
                                         groundFx_.traceP1[0], groundFx_.traceP1[1],
                                         groundFx_.traceP1[2]};
              const auto e = groundTraceEmissiveFast(groundFx_.traceTimeSec, wx, wz, tp);  // Cortana 2026-10-15 1335
              er += e[0]; eg += e[1]; eb += e[2];
            }
            if (er > 0.f || eg > 0.f || eb > 0.f) {
              auto add = [](uint32_t ch, float e) {
                return uint32_t(std::min(255.f, float(ch) + e * 255.f));
              };
              color = 0xFF000000u | (add((color >> 16) & 0xFF, er) << 16) |
                      (add((color >> 8) & 0xFF, eg) << 8) | add(color & 0xFF, eb);
            }
          }
          if (tlQuadMode == kShadeAlphaBlend) {
            // Cortana 2026-10-15 1755 [ENGINE]: blended translucency (Laysor): src*a +
            // dst*(1-a) with the texel's own alpha, depth-tested but no depth write (so
            // nothing behind is lost) and no Hi-Z dirty mark (depth unchanged). value 1 =
            // lit by an Emitor beam: a violet glow added, alpha floored so the light reads.
            // Runs in the per-strip blended pass after the strip's opaque pass; strips are
            // owned by one thread, so the read-modify-write of the pixel is race-free.
            uint32_t a = 255;
            if (fin.texData && fin.varyings.layer < fin.texLayers) {
              const uint32_t tx = uint32_t(swTile(fin.varyings.u) * fin.texScale) & fin.texMask, ty = uint32_t(swTile(fin.varyings.v) * fin.texScale) & fin.texMask;
              a = fin.texData[fin.varyings.layer * fin.texArea + swTexelIndex(fin, tx, ty)] >> 24;
            }
            uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
            if (tlQuadValue) {
              sr = std::min(255u, sr + 70u); sg = std::min(255u, sg + 25u); sb = std::min(255u, sb + 90u);
              a = std::max(a, 150u);
            }
            if (a == 0) continue;
            const uint32_t dst = fb_.pixels[idx], ia = 255u - a;
            const uint32_t r = (sr * a + ((dst >> 16) & 0xFF) * ia + 127u) / 255u;
            const uint32_t g = (sg * a + ((dst >> 8) & 0xFF) * ia + 127u) / 255u;
            const uint32_t b = (sb * a + (dst & 0xFF) * ia + 127u) / 255u;
            fb_.pixels[idx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            continue;
          }
          depth_[idx] = depth;
          fb_.pixels[idx] = color;
          if (x < wxLo) wxLo = x;
          wxHi = x;
        }
      }
    }
    if (hiz && wxHi >= 0) {  // Cortana 2026-10-15 1715: tiles whose depth went down get re-tightened lazily
      uint8_t* d = hizDirty_.data() + size_t(y / kHizTile) * hizTilesX_;
      for (int tx = wxLo / kHizTile; tx <= wxHi / kHizTile; ++tx) d[tx] = 1;
    }
    if (g_swProfOn) {
      if (x1 >= x0) { g_pxScan += uint64_t(x1 - x0 + 1); (simdSpan ? g_pxSimd : g_pxScalar) += uint64_t(x1 - x0 + 1); }
      g_pxCov += lc; g_pxShade += ls;
    }
   }
  }
  if (g_swProfOn) g_tris += uint64_t(nLive);
}

// Build software-vertex-stage varyings from a packed mesh vertex.
// Face normals match the GL chunk shader (chunk.vert N[6]).
// Cortana 2026-10-16 (render.ao): classic voxel corner AO, baked by the mesher into
// PackedVertex.faceAo bits 3-4 (0 = most occluded, 3 = open). Process-wide toggle.
static bool g_swAo = true;
static constexpr float kSwAoLut[4] = {0.55f, 0.70f, 0.85f, 1.0f};
static void varyingsFromPackedVertex(const PackedVertex& v, ShaderVaryings& s) {
  s.ao = g_swAo ? kSwAoLut[(v.faceAo >> 3) & 3] : 1.0f;
  static const float kFaceNormals[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const uint8_t face = v.faceAo & 7;
  s.nx = kFaceNormals[face][0];
  s.ny = kFaceNormals[face][1];
  s.nz = kFaceNormals[face][2];
  s.u = v.u / 16.0f;
  s.v = v.v / 16.0f;
  s.layer = v.layerIndex();  // Cortana 2026-10-15 1628: CA-03 [ENGINE]
  s.wx = v.x / 16.0f;
  s.wy = v.y / 16.0f;
  s.wz = v.z / 16.0f;
  // SEAM (JD-locked coexistence, renderer.h): PackedVertex is 16 bytes fully
  // packed and NEVER extended - it carries no material slot by design, so the
  // classic path's fragments resolve to the 0xFFFF "none" marker and the PBR
  // tier falls back to basic shading per fragment. Real per-vertex material
  // IDs exist on the BinaryQuad path only (PbrQuadVertex::material) and reach
  // the varyings through renderPbrMeshes (P1-RENDER H2).
  s.materialId = 0xFFFF;
}

// Transform all vertices to screen space (x, y, depth, clipW per vertex) in
// the reusable screen_ scratch; returns the scratch pointer.
const float* cSoftwareRenderer::transformVerts(const PackedVertex* verts, size_t count,
                                              const float* viewProj) {
  if (tlPreScreen) { const float* s = tlPreScreen; tlPreScreen = nullptr; return s; }
  static thread_local std::vector<float> tlScreen;  // per band thread (RND-H3)
  tlScreen.resize(count * 4);
  float* screen = tlScreen.data();
  for (size_t i = 0; i < count; ++i) {
    transformVertex(verts[i], viewProj, &screen[i*4], fb_.width, fb_.height);
  }
  return screen;
}

void cSoftwareRenderer::renderFlatQuads(const PackedVertex* verts, size_t count,
                                       const float* viewProj) {
  if (!verts || count < 4 || !viewProj) return;
  const float* screen = transformVerts(verts, count, viewProj);
  // Reject quads with any vertex behind the camera (w <= 0 mirrors projection).
  auto quadVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
           screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
  };
  for (size_t i = 0; i + 3 < count; i += 4) {
    if (!quadVisible(i)) {
      // Cortana 2026-10-15 1420 (Elfie §1.2): the flat/sprite path dropped
      // quads crossing the near plane (RND-H2 fixed only the per-pixel
      // path). Same homogeneous clip against w >= kNearW, then a flat fan.
      // Quads wholly in front never reach here, so their pixels are unchanged.
      constexpr float kNearW = 1.0f / 128.0f;
      float in[4][4], out[8][4];
      int outN = 0;
      bool anyFront = false;
      for (int k = 0; k < 4; ++k) {
        const PackedVertex& pv = verts[i + k];
        const float x = pv.x / 16.0f, y = pv.y / 16.0f, z = pv.z / 16.0f;
        const float* m = viewProj;
        in[k][0] = m[0]*x + m[4]*y + m[8]*z + m[12];
        in[k][1] = m[1]*x + m[5]*y + m[9]*z + m[13];
        in[k][2] = m[2]*x + m[6]*y + m[10]*z + m[14];
        in[k][3] = m[3]*x + m[7]*y + m[11]*z + m[15];
        anyFront |= in[k][3] >= kNearW;
      }
      if (!anyFront) continue;
      for (int k = 0; k < 4; ++k) {
        const float* a = in[k]; const float* b = in[(k + 1) % 4];
        const bool ain = a[3] >= kNearW, bin = b[3] >= kNearW;
        if (ain) { for (int q = 0; q < 4; ++q) out[outN][q] = a[q]; ++outN; }
        if (ain != bin) {
          const float t = (kNearW - a[3]) / (b[3] - a[3]);
          for (int q = 0; q < 4; ++q) out[outN][q] = a[q] + t * (b[q] - a[q]);
          ++outN;
        }
      }
      if (outN < 3) continue;
      float sc[8][4];
      for (int k = 0; k < outN; ++k) {
        const float iw = 1.0f / out[k][3];
        sc[k][0] = (out[k][0] * iw * 0.5f + 0.5f) * fb_.width;
        sc[k][1] = (1.0f - (out[k][1] * iw * 0.5f + 0.5f)) * fb_.height;
        sc[k][2] = out[k][2] * iw;
        sc[k][3] = out[k][3];
      }
      const uint32_t ccol = faceColor(verts[i].faceAo & 0x7, verts[i].value, verts[i].mode);
      for (int k = 1; k + 1 < outN; ++k) rasterizeTriangle(sc[0], sc[k], sc[k + 1], ccol);
      continue;
    }
    uint32_t color = faceColor(verts[i].faceAo & 0x7, verts[i].value, verts[i].mode);
    const float* v0 = &screen[i*4];
    const float* v1 = &screen[(i+1)*4];
    const float* v2 = &screen[(i+2)*4];
    const float* v3 = &screen[(i+3)*4];
    rasterizeTriangle(v0, v1, v2, color);
    rasterizeTriangle(v0, v2, v3, color);
  }
}

void cSoftwareRenderer::renderMesh(const PackedVertex* verts, size_t vertCount,
                                  const float* viewProj) {
  if (!verts || vertCount < 3 || !viewProj) return;

  // Flat path (also the sprite pass path).
  if (!perPixel_) {
    renderFlatQuads(verts, vertCount, viewProj);
    return;
  }

  // Per-pixel path: build varyings from the packed vertices and run the
  // tier-selected fragment shader (Basic = existing basicFragmentShader,
  // Pbr = pbrFragmentShader with graceful fallback per fragment).
  const float* screen = transformVerts(verts, vertCount, viewProj);
  // Reject quads with any vertex behind the camera (w <= 0 mirrors projection).
  auto quadVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
           screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
  };
  const FragmentShaderFn shader = selectFragmentShader(shadingTier_);
  ShaderVaryings sv[4];
  const uint32_t* qlist = tlQuadList;
  const size_t nq = qlist ? tlQuadCount : vertCount / 4;
  tlQuadList = nullptr;
  for (size_t qi = 0; qi < nq; ++qi) {
    const size_t i = qlist ? size_t(qlist[qi]) * 4 : qi * 4;
    if (i + 3 >= vertCount) continue;
    if (tlBlendPass >= 0 && ((verts[i].mode == kShadeAlphaBlend) != (tlBlendPass == 1))) continue;  // Cortana 2026-10-15 1755
    if (!quadVisible(i)) {
      // RND-H2 (2026-10-15): a quad crossing the near plane used to vanish
      // (any w <= 0 rejected the whole quad), so walls/floor beside the
      // camera popped out. Clip it in homogeneous space against
      // w >= kNearW (Sutherland-Hodgman, varyings lerped linearly in clip
      // space, which is exact), then project and fan the result. Quads
      // entirely in front take the fast path above unchanged.
      constexpr float kNearW = 1.0f / 128.0f;
      struct CV { float c[4]; ShaderVaryings s; };
      CV in[4], out[8];
      int inN = 0, outN = 0;
      bool anyFront = false;
      for (int k = 0; k < 4; ++k) {
        const PackedVertex& pv = verts[i + k];
        const float x = pv.x / 16.0f, y = pv.y / 16.0f, z = pv.z / 16.0f;
        const float* m = viewProj;
        CV& cv = in[inN++];
        cv.c[0] = m[0]*x + m[4]*y + m[8]*z + m[12];
        cv.c[1] = m[1]*x + m[5]*y + m[9]*z + m[13];
        cv.c[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
        cv.c[3] = m[3]*x + m[7]*y + m[11]*z + m[15];
        varyingsFromPackedVertex(pv, cv.s);
        anyFront |= cv.c[3] >= kNearW;
      }
      if (!anyFront) continue;
      for (int k = 0; k < inN; ++k) {
        const CV& a = in[k];
        const CV& b = in[(k + 1) % inN];
        const bool ain = a.c[3] >= kNearW, bin = b.c[3] >= kNearW;
        if (ain) out[outN++] = a;
        if (ain != bin) {
          const float t = (kNearW - a.c[3]) / (b.c[3] - a.c[3]);
          CV& r = out[outN++];
          for (int q = 0; q < 4; ++q) r.c[q] = a.c[q] + t * (b.c[q] - a.c[q]);
          r.s = a.s;  // flat fields (layer, materialId) from a
          auto L = [t](float p, float q) { return p + t * (q - p); };
          r.s.nx = L(a.s.nx, b.s.nx); r.s.ny = L(a.s.ny, b.s.ny); r.s.nz = L(a.s.nz, b.s.nz);
          r.s.u = L(a.s.u, b.s.u); r.s.v = L(a.s.v, b.s.v);
          r.s.wx = L(a.s.wx, b.s.wx); r.s.wy = L(a.s.wy, b.s.wy); r.s.wz = L(a.s.wz, b.s.wz);
        }
      }
      if (outN < 3) continue;
      float sc[8][4];
      for (int k = 0; k < outN; ++k) {
        const float iw = 1.0f / out[k].c[3];
        sc[k][0] = (out[k].c[0] * iw * 0.5f + 0.5f) * fb_.width;
        sc[k][1] = (1.0f - (out[k].c[1] * iw * 0.5f + 0.5f)) * fb_.height;
        sc[k][2] = out[k].c[2] * iw;
        sc[k][3] = out[k].c[3];
      }
      const bool cfx = verts[i].mode == kShadeGroundPulse;
      tlQuadMode = verts[i].mode; tlQuadValue = verts[i].value;  // Cortana 2026-10-15 1350
      for (int k = 1; k + 1 < outN; ++k)
        rasterizeTriangleShaded(sc[0], sc[k], sc[k + 1], &out[0].s, &out[k].s, &out[k + 1].s, shader, cfx);
      continue;
    }
    if (tlCullBack) {
      const float* a = &screen[i*4]; const float* b = &screen[(i+1)*4]; const float* c2 = &screen[(i+2)*4];
      const float area = (b[0]-a[0])*(c2[1]-a[1]) - (b[1]-a[1])*(c2[0]-a[0]);
      if (area * g_backSign > 0.f) continue;  // back face: GL would cull it
    }
    {  // RND-H3 band reject (2026-10-15): skip quads wholly outside this
       // thread's rows before building varyings (pure early-out; the
       // rasterizer would cover zero rows of them anyway).
      const float qy0 = std::min(std::min(screen[i*4+1], screen[(i+1)*4+1]), std::min(screen[(i+2)*4+1], screen[(i+3)*4+1]));
      const float qy1 = std::max(std::max(screen[i*4+1], screen[(i+1)*4+1]), std::max(screen[(i+2)*4+1], screen[(i+3)*4+1]));
      if (qy1 < float(tlBandY0) - 1.0f || qy0 > float(tlBandY1) + 1.0f) continue;
      const float qx0 = std::min(std::min(screen[i*4], screen[(i+1)*4]), std::min(screen[(i+2)*4], screen[(i+3)*4]));
      const float qx1 = std::max(std::max(screen[i*4], screen[(i+1)*4]), std::max(screen[(i+2)*4], screen[(i+3)*4]));
      if (tlBandX1 < (1 << 30) || tlBandX0 > 0) {  // step 1 x tile reject (same +-1 slack)
        if (qx1 < float(tlBandX0) - 1.0f || qx0 > float(tlBandX1) + 1.0f) continue;
      }
      if (swP2(kP2CentreCull)) {
        // 2026-10-25 (Archie step 2, sub-pixel diet): the per-triangle centre cull, hoisted to
        // the quad: no pixel centre of this band/tile inside the quad's bbox means neither
        // triangle can cover one, so skip the 4 varyings builds and both setups. Each
        // triangle's bbox lies inside the quad's, so pixels are unchanged.
        constexpr float kEps = 1e-3f;
        const int cx0 = std::max({0, tlBandX0, int(std::ceil(qx0 - 0.5f - kEps))});
        const int cx1 = std::min({int(fb_.width) - 1, tlBandX1 - 1, int(std::floor(qx1 - 0.5f + kEps))});
        const int cy0 = std::max({0, tlBandY0, int(std::ceil(qy0 - 0.5f - kEps))});
        const int cy1 = std::min({int(fb_.height) - 1, tlBandY1 - 1, int(std::floor(qy1 - 0.5f + kEps))});
        if (cx0 > cx1 || cy0 > cy1) { ++tlQuadCentreCulled; continue; }
      }
    }
    for (int k = 0; k < 4; ++k) varyingsFromPackedVertex(verts[i + k], sv[k]);
    // Ground pulse/traces tag: only kShadeGroundPulse quads of a mesh whose
    // baked params renderFrame supplied (direct renderMesh callers leave
    // groundFx_ zeroed, so their frames are unchanged).
    const bool fx = verts[i].mode == kShadeGroundPulse;
    tlQuadMode = verts[i].mode; tlQuadValue = verts[i].value;  // Cortana 2026-10-15 1350
    if (swP2(kP2Quad)) {  // Ginny (Argon) Pass 2 step 1: one setup + one row walk per quad
      const float* tv[2][3] = {{&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4]},
                               {&screen[i*4], &screen[(i+2)*4], &screen[(i+3)*4]}};
      const ShaderVaryings* ts[2][3] = {{&sv[0], &sv[1], &sv[2]}, {&sv[0], &sv[2], &sv[3]}};
      rasterizeTrisShaded(2, tv, ts, shader, fx);
      continue;
    }
    rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                            &sv[0], &sv[1], &sv[2], shader, fx);
    rasterizeTriangleShaded(&screen[i*4], &screen[(i+2)*4], &screen[(i+3)*4],
                            &sv[0], &sv[2], &sv[3], shader, fx);
  }
  tlQuadMode = 0; tlQuadValue = 0;  // Cortana 2026-10-15 1350: entity/PBR passes shade plain
}

void cSoftwareRenderer::renderEntityVerts(const float* viewProj) {
  const PackedVertex* verts = entityVerts_.data();
  const size_t count = entityVerts_.size();
  if (count < 3 || !viewProj) return;
  const float* screen = transformVerts(verts, count, viewProj);
  auto triVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 && screen[(i+2)*4+3] > 0;
  };
  if (perPixel_) {
    const FragmentShaderFn shader = selectFragmentShader(shadingTier_);
    ShaderVaryings sv[3];
    for (size_t i = 0; i + 2 < count; i += 3) {
      if (!triVisible(i)) continue;
      for (int k = 0; k < 3; ++k) varyingsFromPackedVertex(verts[i + k], sv[k]);
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                              &sv[0], &sv[1], &sv[2], shader);
    }
    return;
  }
  for (size_t i = 0; i + 2 < count; i += 3) {
    if (!triVisible(i)) continue;
    const uint32_t color = faceColor(verts[i].faceAo & 0x7, verts[i].value, verts[i].mode);
    rasterizeTriangle(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4], color);
  }
}

void cSoftwareRenderer::setTextures(const uint32_t* data, uint32_t layers) {
  textures_.setRaw(data, layers);
}

void cSoftwareRenderer::applyFXAA() {
  // FXAA-lite (P3-3, Appendix D L2): a 3x3 box blend applied ONLY at
  // high-contrast edges — a pixel blends toward its neighbourhood average
  // when the neighbourhood's luminance range exceeds kEdgeThreshold;
  // flat regions pass through untouched. (The v1 code blurred EVERYTHING
  // despite the "edges only" comment — dead-ish and misnamed; this gate is
  // what the name claims. It is still not a full FXAA shader: no luma
  // subpixel/edge-direction search. No production caller yet — the direct
  // CPU surface and tests drive it.)
  // fxaaSrc_ is a reusable member buffer (no per-call full-framebuffer copy).
  constexpr uint32_t kEdgeThreshold = 48;  // luminance range (0..255) that counts as an edge
  auto lum = [](uint32_t p) {
    return (77u * ((p >> 16) & 0xFF) + 150u * ((p >> 8) & 0xFF) + 29u * (p & 0xFF)) >> 8;
  };
  fxaaSrc_ = fb_.pixels;
  const std::vector<uint32_t>& src = fxaaSrc_;
  uint32_t w = fb_.width, h = fb_.height;
  for (uint32_t y = 1; y + 1 < h; ++y) {
    for (uint32_t x = 1; x + 1 < w; ++x) {
      uint32_t r = 0, g = 0, b = 0, lo = 255, hi = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          uint32_t p = src[size_t(y+dy) * w + (x+dx)];
          r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF;
          const uint32_t l = lum(p);
          lo = std::min(lo, l);
          hi = std::max(hi, l);
        }
      }
      if (hi - lo < kEdgeThreshold) continue;  // not an edge: keep the source pixel
      fb_.pixels[size_t(y) * w + x] = 0xFF000000 | ((r/9) << 16) | ((g/9) << 8) | (b/9);
    }
  }
}

// ---------------------------------------------------------------------------
// IRenderer interface
// ---------------------------------------------------------------------------

std::string cSoftwareRenderer::deviceInfo() const {
  return "software rasterizer (CPU, no GPU)";
}

void cSoftwareRenderer::setMaterials(const BlockMaterials& materials) {
  // 2026-10-16 (JD bug 1): the colour-lamp palette rows and mask layers (kShadePalette16/32/64),
  // as GL33 / Vulkan bind them. PBR material data still arrives via setPbrContext().
  const MaterialBindings& b = materials.bindings();
  lampPal_ = b.tierPalette; lampMask_ = b.tierMask; lampRim_ = b.tierRim;
  leafGlow_ = b.leafGlow;  // 2026-10-16 crystal twinkle
  tintMask_ = b.tintMask; stripPal_ = b.stripPalette;  // 2026-10-16 kShadeTraceTint
}

int cSoftwareRenderer::loadShaders(const ShaderLibrary& /*shaders*/) {
  return 0;  // no GPU programs; no program can use pack sources
}

// Mesh key identical to the GL/Vulkan backends.
static int64_t swMeshKey(int cx, int sy, int cz) {
  return int64_t((uint64_t(uint32_t(cx)) << 40) ^ (uint64_t(uint32_t(sy)) << 32) ^
                 uint64_t(uint32_t(cz)));
}

// Ginny (Argon) 2026-10-16 Pass 2 step 3: classify each quad into a face-direction bucket
// from its own winding (exact integer cross product in 1/16-block units), so the bucket
// agrees with the screen-space winding cull by construction.
static void swFaceBuckets(const std::vector<PackedVertex>& v, std::vector<uint8_t>& face, float* lim) {
  const size_t nq = v.size() / 4;
  face.assign(nq, uint8_t(6));
  for (int b = 0; b < 6; ++b) lim[b] = (b & 1) ? -1e30f : 1e30f;
  for (size_t q = 0; q < nq; ++q) {
    const PackedVertex* a = &v[q * 4];
    const int64_t e1[3] = {int64_t(a[1].x) - a[0].x, int64_t(a[1].y) - a[0].y, int64_t(a[1].z) - a[0].z};
    const int64_t e2[3] = {int64_t(a[2].x) - a[0].x, int64_t(a[2].y) - a[0].y, int64_t(a[2].z) - a[0].z};
    const int64_t n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    int axis = -1;
    for (int k = 0; k < 3; ++k)
      if (n[k] != 0 && n[(k + 1) % 3] == 0 && n[(k + 2) % 3] == 0) axis = k;
    if (axis < 0) continue;
    auto c = [&](int i) { return axis == 0 ? a[i].x : axis == 1 ? a[i].y : a[i].z; };
    if (c(1) != c(0) || c(2) != c(0) || c(3) != c(0)) continue;  // not planar on the axis
    // second triangle (v0 v2 v3) must wind the same way (convex, non-degenerate quad)
    const int64_t f1[3] = {e2[0], e2[1], e2[2]};
    const int64_t f2[3] = {int64_t(a[3].x) - a[0].x, int64_t(a[3].y) - a[0].y, int64_t(a[3].z) - a[0].z};
    const int64_t m = axis == 0 ? f1[1] * f2[2] - f1[2] * f2[1] : axis == 1 ? f1[2] * f2[0] - f1[0] * f2[2] : f1[0] * f2[1] - f1[1] * f2[0];
    if ((m > 0) != (n[axis] > 0) || m == 0) continue;
    const int b = axis * 2 + (n[axis] > 0 ? 0 : 1);
    face[q] = uint8_t(b);
    const float plane = float(c(0)) / 16.0f;
    lim[b] = (b & 1) ? std::max(lim[b], plane) : std::min(lim[b], plane);
  }
}

// Elfie 2026-10-18 (Argon counsel pt 1, phase B): pack the ordered quad vector
// into StoredQuad form. Shared-attribute invariance is PROVEN for mesher output
// (mesher.cpp emitFace builds all 4 corners from one Look) and CHECKED here
// quad by quad, so any producer stays correct: a single violating quad keeps
// the whole mesh on the legacy PackedVertex store. The 24-bit slot is the one
// field not stored: no software consumer reads it (renderer.h), and expansion
// reconstructs it as 0.
bool cSoftwareRenderer::packQuads(const std::vector<PackedVertex>& ordered,
                                 std::vector<StoredQuad>& out) {
  if (ordered.size() % 4 != 0) return false;
  // CREW TILE (shake 2026-10-18): build into `out` (the caller's store
  // vector) instead of a fresh local moved into it, so re-uploads of a key
  // reuse the store's capacity. The old untouched-on-failure contract is
  // preserved explicitly: the failure path below clears `out` again.
  out.clear();
  out.reserve(ordered.size() / 4);
  for (size_t q = 0; q < ordered.size(); q += 4) {
    const PackedVertex& a = ordered[q];
    const uint16_t layer = uint16_t(uint32_t(a.layer) | (uint32_t(a.layerHi) << 8));
    for (int k = 1; k < 4; ++k) {
      const PackedVertex& b = ordered[q + k];
      if (b.layer != a.layer || b.layerHi != a.layerHi || b.value != a.value ||
          b.mode != a.mode || (b.faceAo & 0x67) != (a.faceAo & 0x67)) {
        out.clear();  // CREW TILE: preserve untouched-on-failure (see above)
        return false;  // face (bits 0-2) + rot (bits 5-6) must be quad-shared
      }
    }
    StoredQuad sq;
    sq.layer = layer;
    sq.value = a.value;
    sq.mode = a.mode;
    sq.faceRot = uint8_t(a.faceAo & 0x67);
    uint8_t ao = 0;
    for (int k = 0; k < 4; ++k) {
      const PackedVertex& c = ordered[q + k];
      ao = uint8_t(bitops::insertBits32(ao, c.faceAo >> 3, unsigned(2 * k), 2));  // build 59: bit-exact AO field insert (tools/bitops_diff_test.cpp)
      sq.corners[k] = {c.x, c.y, c.z, c.u, c.v};
    }
    sq.aoPack = ao;
    out.push_back(sq);
  }
  return true;
}

// Byte-identical inverse of packQuads for the drawn range; legacy meshes copy.
void cSoftwareRenderer::expandStored(const StoredMesh& m, size_t vertBegin, size_t count,
                                    PackedVertex* dst) {
  if (m.quads.empty()) {
    if (count) std::memcpy(dst, m.verts.data() + vertBegin, count * sizeof(PackedVertex));
    return;
  }
  const size_t qBegin = vertBegin / 4;
  const size_t qEnd = (vertBegin + count + 3) / 4;
  for (size_t q = qBegin; q < qEnd; ++q) {
    const StoredQuad& sq = m.quads[q];
    for (int k = 0; k < 4; ++k) {
      const size_t vi = q * 4 + size_t(k);
      if (vi < vertBegin || vi >= vertBegin + count) continue;
      PackedVertex& v = dst[vi - vertBegin];
      v.x = sq.corners[k].x;
      v.y = sq.corners[k].y;
      v.z = sq.corners[k].z;
      v.u = sq.corners[k].u;
      v.v = sq.corners[k].v;
      v.faceAo = uint8_t(sq.faceRot | (bitops::extractBits32(sq.aoPack, unsigned(2 * k), 2) << 3));  // build 59: bit-exact AO field extract (tools/bitops_diff_test.cpp)
      v.layer = uint8_t(sq.layer & 0xFF);
      v.value = sq.value;
      v.mode = sq.mode;
      v.slot = 0;  // dropped at pack time; no software consumer reads it
      v.layerHi = uint8_t(sq.layer >> 8);
    }
  }
}

void cSoftwareRenderer::uploadChunkMesh(const ChunkMesh& mesh) {
  ++storeGen_;  // mesh-store seal (Design B Stage 0): every mutation bumps the generation
  const int64_t key = swMeshKey(mesh.cx, mesh.sy, mesh.cz);
  if (mesh.verts.empty()) {  // empty = remove, like the other backends
    meshes_.erase(key);
    return;
  }
  StoredMesh& s = meshes_[key];
  s.cx = mesh.cx;
  s.sy = mesh.sy;
  s.cz = mesh.cz;
  {  // Cortana 2026-10-16 (render.emissive_light): one emitter per lit block cell (render-only).
    s.emit.clear();
    std::vector<uint32_t> seen;
    for (size_t q = 0; q + 3 < mesh.verts.size() && s.emit.size() < 64 * 6; q += 4) {
      const PackedVertex& v = mesh.verts[q];
      float r, g, b;
      if (v.mode == kShadeEmissive) { r = 1.0f; g = 0.85f; b = 0.6f; }
      else if (v.mode == kShadeStripTint0 && v.value > 0) { r = 1.0f; g = 0.22f; b = 0.18f; }
      else if (v.mode == kShadeStripTint1 && v.value > 0) { r = 0.25f; g = 0.45f; b = 1.0f; }
      else if (v.mode == kShadeTraceTint && ((v.value & 0x80) || ((llP2PerfOff() & 8) && v.value > 0))) { r = 1.0f; g = 0.35f; b = 0.3f; }  // 1520: lit bit only (Via slot index made every Via an emitter)
      else continue;
      float cx = 0, cy = 0, cz = 0;
      for (int k = 0; k < 4; ++k) { cx += mesh.verts[q + k].x; cy += mesh.verts[q + k].y; cz += mesh.verts[q + k].z; }
      cx /= 64.f; cy /= 64.f; cz /= 64.f;  // /4 corners, /16 sub-block units
      const uint32_t cell = (uint32_t(int(cx)) & 31u) | ((uint32_t(int(cy)) & 31u) << 5) | ((uint32_t(int(cz)) & 31u) << 10);
      if (std::find(seen.begin(), seen.end(), cell) != seen.end()) continue;
      seen.push_back(cell);
      const float e[6] = {mesh.cx * 16.f + cx, mesh.sy * 16.f + cy, mesh.cz * 16.f + cz, r, g, b};
      s.emit.insert(s.emit.end(), e, e + 6);
    }
  }
  // Cortana 2026-10-16: LOD-tagged quads (foliage) split into a near set (detail) and a far set.
  // The far set keeps only untagged quads: far foliage is skipped on the software path.
  // Elfie 2026-10-17 (Argon counsel pt 1, phase A): the two sets are no longer both stored.
  // One vector, reordered: untagged quads first (original relative order — the exact old far
  // set), then kShadeLodNear quads with tag bits masked (with the prefix, the exact old near
  // set selectLod produced). kShadeLodFar quads were dropped by BOTH old sets on this path,
  // so they are not stored. Far mode draws the prefix [0, untaggedCount) quads only.
  s.hasLod = hasLodTags(mesh.verts);
  // Elfie 2026-10-18 phase B: the phase-A ordered set is built in a local,
  // then packed into StoredQuad form (legacy store kept if packing declines).
  // CREW TILE (shake 2026-10-18): the no-LOD common path used to copy the
  // whole mesh into `ordered` just to read it back below. Readers now work
  // on mesh.verts directly; only the LOD path builds a reordered set, in a
  // reused member scratch. Contents seen by every consumer are unchanged.
  const std::vector<PackedVertex>* orderedPtr = &mesh.verts;
  if (s.hasLod) {
    std::vector<PackedVertex>& ordered = uploadOrderedScratch_;
    ordered.clear();
    ordered.reserve(mesh.verts.size());
    size_t untagged = 0;
    for (size_t q = 0; q + 3 < mesh.verts.size(); q += 4)
      if (!(mesh.verts[q].mode & (kShadeLodNear | kShadeLodFar))) {
        ordered.insert(ordered.end(), mesh.verts.begin() + q, mesh.verts.begin() + q + 4);
        ++untagged;
      }
    for (size_t q = 0; q + 3 < mesh.verts.size(); q += 4)
      if (mesh.verts[q].mode & kShadeLodNear)
        for (size_t k = 0; k < 4; ++k) {
          PackedVertex v = mesh.verts[q + k];
          v.mode = uint8_t(v.mode & kShadeModeMask);
          ordered.push_back(v);
        }
    s.untaggedCount = untagged;
    orderedPtr = &ordered;
  } else {
    s.untaggedCount = mesh.verts.size() / 4;
  }
  const std::vector<PackedVertex>& ordered = *orderedPtr;
  swFaceBuckets(ordered, s.quadFace, s.planeLim);  // Pass 2 step 3 (one set now)
  s.hasBlend = false;  // Cortana 2026-10-15 1755: route to the blended pass
  for (size_t q = 0; q < ordered.size(); q += 4)
    if (ordered[q].mode == kShadeAlphaBlend) { s.hasBlend = true; break; }
  if (packQuads(ordered, s.quads)) {
    s.verts.clear();
    s.verts.shrink_to_fit();  // release the legacy buffer: memory is the point
  } else {
    s.quads.clear();
    // CREW TILE: `ordered` is mesh.verts (no-LOD) or the member scratch
    // (LOD) now -- copy in the first case, move in the second, exactly the
    // values the old local held either way.
    if (s.hasLod) s.verts = std::move(uploadOrderedScratch_);
    else s.verts = mesh.verts;
  }
  for (int i = 0; i < 4; ++i) {
    s.pulseP0[i] = mesh.pulseP0[i];
    s.pulseP1[i] = mesh.pulseP1[i];
    s.traceP0[i] = mesh.traceP0[i];
    s.traceP1[i] = mesh.traceP1[i];
  }
}

// --- PBR / culling surface (P1-RENDER H2): real implementations, no inherited
// no-ops left on the paths the game calls. -----------------------------------

void cSoftwareRenderer::setPbrMaterials(const cMaterialReg& reg) {
  pbrMaterials_ = &reg;
  ownedPbrCtx_.registry = &reg;
  ownedPbrCtx_.ambientMode = ambientMode_;
  ownedPbrCtx_.emissive = &frameLights_;  // FrameParams::pbrLights, fed per frame
}

void cSoftwareRenderer::setPbrTier(PbrTransparencyTier t) {
  pbrTier_ = t;
  // Selecting a PBR transparency tier is the user's PBR opt-in: run the PBR
  // fragment tier on the per-pixel path. Classic PackedVertex fragments carry
  // no material ID (0xFFFF) and fall back to basic per fragment, so classic
  // frames are pixel-identical; BinaryQuad fragments shade real PBR.
  shadingTier_ = SoftwareShadingTier::Pbr;
  // Honest limit (pbr_software_shaders.h ULTRA TIER GAP): the CPU path does
  // not implement the transparency-tier ladder — PBR fragments shade
  // opaque regardless of tier. Loud once, not silent.
  static bool warnedTier = false;
  if (!warnedTier) {
    warnedTier = true;
    logWarn("software: PBR transparency tiers are not implemented on the CPU path; "
            "PBR fragments shade opaque (tier selection still enables the PBR fragment path)");
  }
}

void cSoftwareRenderer::setAmbientMode(AmbientMode m) {
  ambientMode_ = m;
  ownedPbrCtx_.ambientMode = m;
}

void cSoftwareRenderer::setFrustumMarginDeg(float degrees) {
  frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11, as GL/VK
}

void cSoftwareRenderer::setModelDither(bool enabled, float nearBlocks, float farBlocks) {
  ditherEnabled_ = enabled;
  ditherNear_ = std::clamp(nearBlocks, 0.f, 1024.f);
  ditherFar_ = std::clamp(farBlocks, ditherNear_, 2048.f);
}

// ---------------------------------------------------------------------------
// AA ladder + ASCII pipeline (JD 2026-10-13). Both are presentational
// stages on the finished frame; neither touches sim or mesh state.
// ---------------------------------------------------------------------------
SoftwareAaScratch& cSoftwareRenderer::ensureAaScratch() {
  if (!aaScratch_) aaScratch_ = std::make_unique<SoftwareAaScratch>();
  return *aaScratch_;
}

void cSoftwareRenderer::setAo(bool on) { g_swAo = on; }  // Cortana 2026-10-16: render.ao

void cSoftwareRenderer::setAaMode(const char* modeName) {
  SoftwareAaMode mode;
  if (modeName && softwareAaModeFromName(modeName, mode)) aaMode_ = mode;
}

const char* cSoftwareRenderer::aaModeName() const { return softwareAaModeName(aaMode_); }

void cSoftwareRenderer::applyAaPostPass() {
  if (aaMode_ == SoftwareAaMode::Fxaa) {
    // Cortana 2026-10-16 (texture-preserving AA): depth gates FXAA to geometry edges.
    applyFxaaPost(fb_, ensureAaScratch(), (depth_.size() == fb_.pixels.size()) ? depth_.data() : nullptr);
  } else if (aaMode_ == SoftwareAaMode::Cmaa) {
    // depth_ matches fb_ on the direct surface (resize keeps them paired);
    // pass it only when the pairing holds, else run luma-only.
    const float* depth =
        (depth_.size() == fb_.pixels.size()) ? depth_.data() : nullptr;
    applyCmaaPost(fb_, depth, ensureAaScratch());
  }
  // Off: no-op. Ssaa2x: render-time mode — nothing to post-apply.
}

void cSoftwareRenderer::setAsciiMode(bool enabled, int cols) {
  asciiEnabled_ = enabled;
  asciiCols_ = std::clamp(cols, 40, 400);
}

void cSoftwareRenderer::setAsciiGlyphSet(const char* setName) {
  AsciiGlyphSet set;
  if (setName && asciiGlyphSetFromName(setName, set)) asciiSet_ = static_cast<int>(set);
}

void cSoftwareRenderer::setAsciiValueSpan(int span) {
  asciiSpan_ = std::clamp(span, 1, kAsciiMaxValueSpan);  // JD amendment 2026-10-13
}

void cSoftwareRenderer::applyAsciiPass() {
  framebufferToAsciiImage(fb_, asciiCols_, static_cast<AsciiGlyphSet>(asciiSet_),
                          &asciiText_, nullptr, asciiSpan_);
}

void cSoftwareRenderer::setCullProtoConfig(const CullProtoConfig& cfg) {
  cullProto_.configure(cfg);
}

CullProtoStats cSoftwareRenderer::cullProtoStats() const { return cullProto_.stats(); }

void cSoftwareRenderer::uploadPbrChunkMesh(const PbrChunkMesh& mesh) {
  ++storeGen_;  // mesh-store seal (Design B Stage 0)
  const int64_t key = swMeshKey(mesh.cx, mesh.sy, mesh.cz);
  if (mesh.verts.empty()) {  // empty = remove, like the classic store
    pbrMeshes_.erase(key);
    return;
  }
  pbrMeshes_[key] = mesh;
}

void cSoftwareRenderer::uploadEntityMesh(const std::vector<PackedVertex>& verts) {
  entityVerts_ = verts;  // dynamic: each upload replaces the previous batch
}

void cSoftwareRenderer::clearChunkMeshes() {
  ++storeGen_;  // mesh-store seal (Design B Stage 0)
  meshes_.clear();  // entity verts kept, matching GL/Vulkan clear semantics
  pbrMeshes_.clear();
}

static Mat4 swTranslation(float x, float y, float z) {
  Mat4 t = Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

// FrameParams sky (0..1 floats) as the framebuffer's 0xAARRGGBB clear color:
// sky {0.58, 0.70, 0.86} -> 0xFF94B3DB, the wafer day sky the MCP capture
// path clears with.
static uint32_t swSkyColor(const Vec3& sky) {
  auto ch = [](float c) {
    return uint32_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  return 0xFF000000u | (ch(sky.x) << 16) | (ch(sky.y) << 8) | ch(sky.z);
}

void cSoftwareRenderer::rasterizeBlendSprite(const BlendSprite& s) {
  // Two triangles (0,1,2) + (0,2,3), barycentric like rasterizeTriangle,
  // with backface culling, a depth TEST against the world depth buffer,
  // and NO depth write: a nearer blended sprite must not occlude a farther
  // one drawn after it (producer order is back-to-front).
  const int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (const auto& t : tris) {
    const float* v0 = s.v[t[0]];
    const float* v1 = s.v[t[1]];
    const float* v2 = s.v[t[2]];
    if (isBackface(v0, v1, v2)) continue;
    const int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
    const int x1 = std::min(int(fb_.width) - 1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
    const int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
    const int y1 = std::min(int(fb_.height) - 1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));
    auto edgeFn = [](const float* a, const float* b, float x, float y) {
      return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
    };
    const float area = edgeFn(v0, v1, v2[0], v2[1]);
    if (std::abs(area) < 1e-9f) continue;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float px = x + 0.5f, py = y + 0.5f;
        const float w0 = edgeFn(v1, v2, px, py) / area;
        const float w1 = edgeFn(v2, v0, px, py) / area;
        const float w2 = edgeFn(v0, v1, px, py) / area;
        if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
        const float depth = w0 * v0[2] + w1 * v1[2] + w2 * v2[2];
        const size_t idx = size_t(y) * fb_.width + size_t(x);
        if (!(depth < depth_[idx])) continue;  // test only; depth_ untouched
        const uint32_t dst = fb_.pixels[idx];
        const float dr = float((dst >> 16) & 0xFF), dg = float((dst >> 8) & 0xFF),
                    db = float(dst & 0xFF);
        float r, g, b;
        if (s.additive) {
          r = dr + s.r * s.a * 255.f;
          g = dg + s.g * s.a * 255.f;
          b = db + s.b * s.a * 255.f;
        } else {
          r = dr + (s.r * 255.f - dr) * s.a;
          g = dg + (s.g * 255.f - dg) * s.a;
          b = db + (s.b * 255.f - db) * s.a;
        }
        auto ch = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
        fb_.pixels[idx] = 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
      }
    }
  }
}

void cSoftwareRenderer::drawSprites(const FrameParams& p) {
  if (!p.spriteQuads || p.spriteQuadCount == 0) return;
  // The game sprite host's software conversion (sprite_host.cpp), mirrored:
  // each SpriteDrawQuad becomes 4 PackedVerts relative to a 16-aligned
  // origin 1024 blocks below the camera on each axis (so live sprites land
  // inside the uint16 local range on the positive side; quads outside are
  // skipped), shaded flat with the luminance of the sprite's atlas-average
  // color x tint on face 2 (faceColor's unshaded face). The host averages
  // the def's frame-0 atlas rect; here the average is taken over the quad's
  // own uv rect on its atlas page — the same region for frame 0, and the
  // drawn frame's region for later animation frames.
  const Vec3 origin{std::floor((p.camPos.x - 1024.f) / 16.f) * 16.f,
                    std::floor((p.camPos.y - 1024.f) / 16.f) * 16.f,
                    std::floor((p.camPos.z - 1024.f) / 16.f) * 16.f};
  // Average-color cache (D-M2): keyed by (atlas version, page, pixel
  // rect); a version move clears it wholesale. The average itself is
  // summed directly over the page rect — no per-quad temp Image/vector.
  if (spriteAvgVersion_ != p.spriteAtlasVersion) {
    spriteAvgCache_.clear();
    spriteAvgVersion_ = p.spriteAtlasVersion;
  }
  auto avgColorOf = [&](const SpriteDrawQuad& q) -> std::array<float, 4> {
    std::array<float, 4> avg{1.f, 1.f, 1.f, 1.f};  // fallback: white (as the host)
    if (q.page < 0 || size_t(q.page) >= p.spriteAtlasPages.size()) return avg;
    const Image& page = p.spriteAtlasPages[size_t(q.page)];
    if (page.empty() || page.width() <= 0 || page.height() <= 0) return avg;
    float u0 = q.uv[0][0], v0 = q.uv[0][1], u1 = u0, v1 = v0;
    for (int i = 1; i < 4; ++i) {
      u0 = std::min(u0, q.uv[i][0]);
      v0 = std::min(v0, q.uv[i][1]);
      u1 = std::max(u1, q.uv[i][0]);
      v1 = std::max(v1, q.uv[i][1]);
    }
    const int x0 = std::clamp(int(std::lround(u0 * float(page.width()))), 0, page.width());
    const int y0 = std::clamp(int(std::lround(v0 * float(page.height()))), 0, page.height());
    const int x1 = std::clamp(int(std::lround(u1 * float(page.width()))), 0, page.width());
    const int y1 = std::clamp(int(std::lround(v1 * float(page.height()))), 0, page.height());
    if (x1 <= x0 || y1 <= y0) return avg;
    const SpriteAvgKey key{p.spriteAtlasVersion, q.page, x0, y0, x1, y1};
    if (const auto it = spriteAvgCache_.find(key); it != spriteAvgCache_.end()) return it->second;
    // Same weighting as Image::averageColor (alpha-weighted RGB), plus the
    // plain mean alpha the blend modes need.
    double s[3] = {0, 0, 0}, wsum = 0, asum = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const uint8_t* px = page.pixel(x, y);
        const double a = px[3] / 255.0;
        for (int c = 0; c < 3; ++c) s[c] += a * px[c] / 255.0;
        wsum += a;
        asum += a;
      }
    if (wsum > 0) {
      const double n = double(x1 - x0) * double(y1 - y0);
      avg = {float(s[0] / wsum), float(s[1] / wsum), float(s[2] / wsum), float(asum / n)};
    }
    spriteAvgCache_.emplace(key, avg);
    return avg;
  };
  spriteScratch_.clear();
  spriteScratch_.reserve(p.spriteQuadCount * 4);
  spriteBlendScratch_.clear();
  const Mat4 mvp = mul(p.viewProj, swTranslation(origin.x, origin.y, origin.z));
  for (size_t qi = 0; qi < p.spriteQuadCount; ++qi) {
    const SpriteDrawQuad& q = p.spriteQuads[qi];
    const std::array<float, 4> avg = avgColorOf(q);
    PackedVertex pv[4];
    bool inRange = true;
    for (int i = 0; i < 4; ++i) {
      const float lx = (q.corners[i].x - origin.x) * 16.f;
      const float ly = (q.corners[i].y - origin.y) * 16.f;
      const float lz = (q.corners[i].z - origin.z) * 16.f;
      if (lx < 0.f || ly < 0.f || lz < 0.f || lx > 65535.f || ly > 65535.f || lz > 65535.f)
        inRange = false;
      pv[i].x = uint16_t(std::clamp(lx, 0.f, 65535.f));
      pv[i].y = uint16_t(std::clamp(ly, 0.f, 65535.f));
      pv[i].z = uint16_t(std::clamp(lz, 0.f, 65535.f));
      pv[i].u = 0;
      pv[i].v = 0;
      pv[i].faceAo = 2;  // face 2 = top: faceColor's unshaded (1.0) face
      pv[i].layer = 0;
      pv[i].mode = kShadeTex;
      pv[i].slot = 0;
    }
    if (!inRange) continue;
    const bool blended = q.shade == kShadeAlphaBlend || q.shade == kShadeAdditive;
    if (!blended) {
      const float lum = std::clamp(0.2126f * avg[0] * q.r + 0.7152f * avg[1] * q.g +
                                       0.0722f * avg[2] * q.b,
                                   0.f, 1.f);
      for (PackedVertex& v : pv) v.value = uint8_t(std::lround(lum * 255.f));
      for (const PackedVertex& v : pv) spriteScratch_.push_back(v);
    } else {
      // Blended: keep the quad's TRUE average color x tint (the flat
      // luminance collapse is the opaque path's documented form; blending
      // a gray would lose the sprite's hue for no benefit).
      BlendSprite bs;
      bs.r = std::clamp(avg[0] * q.r, 0.f, 1.f);
      bs.g = std::clamp(avg[1] * q.g, 0.f, 1.f);
      bs.b = std::clamp(avg[2] * q.b, 0.f, 1.f);
      bs.a = std::clamp(avg[3] * q.a, 0.f, 1.f);
      bs.additive = q.shade == kShadeAdditive;
      if (ditherEnabled_) {
        // Model dithering for blended sprites (JD 2026-10-13): fade the
        // whole billboard by its NEAREST corner's distance to the camera —
        // per-sprite, not per-pixel (the opaque paths dither per-pixel in
        // the rasterizers; a translucent quad reads better as a fade).
        float dmin = 1e30f;
        for (int i = 0; i < 4; ++i) {
          const float dx = q.corners[i].x - p.camPos.x;
          const float dy = q.corners[i].y - p.camPos.y;
          const float dz = q.corners[i].z - p.camPos.z;
          dmin = std::min(dmin, sqrtFast(dx * dx + dy * dy + dz * dz));  // Carmack sqrt (dither fade distance)
        }
        bs.a *= modelDitherFade(dmin, ditherNear_, ditherFar_);
        if (bs.a <= 0.f) continue;  // fully faded: skip the quad entirely
      }
      const float* screen = transformVerts(pv, 4, mvp.m);
      bool visible = true;
      for (int i = 0; i < 4; ++i) {
        if (screen[i * 4 + 3] <= 0) visible = false;
        for (int k = 0; k < 4; ++k) bs.v[i][k] = screen[i * 4 + k];
      }
      if (visible) spriteBlendScratch_.push_back(bs);
    }
  }
  // Producer order is cutout-first, then blended back-to-front: drawing
  // the opaque batch first and the blended quads in order reproduces the
  // FrameParams contract exactly.
  if (!spriteScratch_.empty()) renderFlatQuads(spriteScratch_.data(), spriteScratch_.size(), mvp.m);
  for (const BlendSprite& bs : spriteBlendScratch_) rasterizeBlendSprite(bs);
}

void cSoftwareRenderer::renderPbrMeshes(const FrameParams& p) {
  // BinaryQuad (PBR) meshes: positions convert to PackedVertex-shaped verts
  // for the shared transform; the varyings carry the REAL per-vertex compact
  // material ID (PbrQuadVertex::material) — the fix for the classic path's
  // 0xFFFF seam (H2). Shading is pbrFragmentShader through the effective
  // context (external setPbrContext, else the owned setPbrMaterials one);
  // with neither, it falls back to basic per fragment, never black.
  frameLights_.frame = &p;
  if (pbrMaterials_) {
    ownedPbrCtx_.camX = p.camPos.x;
    ownedPbrCtx_.camY = p.camPos.y;
    ownedPbrCtx_.camZ = p.camPos.z;
  }
  for (const auto& [key, m] : pbrMeshes_) {
    (void)key;
    const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
    if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos,
                          frustumMarginDeg_) != CullDecision::Full)
      continue;
    const Mat4 mvp = mul(p.viewProj, swTranslation(mn.x, mn.y, mn.z));
    pbrScratch_.clear();
    pbrScratchMat_.clear();
    pbrScratch_.reserve(m.verts.size());
    pbrScratchMat_.reserve(m.verts.size());
    for (const PbrQuadVertex& v : m.verts) {
      PackedVertex pv{};
      pv.x = v.x; pv.y = v.y; pv.z = v.z;
      pv.faceAo = uint8_t(v.face & 7);
      pbrScratch_.push_back(pv);
      pbrScratchMat_.push_back(v.material);
    }
    const float* screen = transformVerts(pbrScratch_.data(), pbrScratch_.size(), mvp.m);
    auto quadVisible = [&](size_t i) {
      return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
             screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
    };
    static const float kFaceNormals[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    ShaderVaryings sv[4];
    for (size_t i = 0; i + 3 < pbrScratch_.size(); i += 4) {
      if (!quadVisible(i)) continue;
      // Per-quad flood-fill levels ride the owned context (its block/sky
      // fields are context-global; the rasterizer is synchronous, so
      // setting them per quad is exact). External contexts are the
      // caller's own — theirs stay untouched.
      if (!pbrContext_ && pbrMaterials_) {
        ownedPbrCtx_.blockLight = m.verts[i].blockLight;
        ownedPbrCtx_.skyLight = m.verts[i].skyLight;
      }
      for (int k = 0; k < 4; ++k) {
        const PbrQuadVertex& v = m.verts[i + k];
        const uint32_t face = v.face < 6 ? v.face : 2;
        sv[k].nx = kFaceNormals[face][0];
        sv[k].ny = kFaceNormals[face][1];
        sv[k].nz = kFaceNormals[face][2];
        sv[k].u = 0.f; sv[k].v = 0.f; sv[k].layer = 0;
        sv[k].wx = mn.x + v.x / 16.f;  // world position (view dir + lights)
        sv[k].wy = mn.y + v.y / 16.f;
        sv[k].wz = mn.z + v.z / 16.f;
        sv[k].materialId = uint16_t(pbrScratchMat_[i + k] & 0xFFFFu);
      }
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                              &sv[0], &sv[1], &sv[2], &pbrFragmentShader);
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+2)*4], &screen[(i+3)*4],
                              &sv[0], &sv[2], &sv[3], &pbrFragmentShader);
    }
    ++stats_.drawn;
  }
  frameLights_.frame = nullptr;
}

// ---------------------------------------------------------------------------
// HUD overlay (P1-RENDER H3): CPU rasterizer for buildHudVertices output.
// Mirrors the GL33 hud program: pixel-space triangles, no depth test,
// src-over blending in paint order; u < 0 = solid rect, otherwise the font
// atlas coverage (NEAREST, like GL) multiplies the vertex alpha.
// ---------------------------------------------------------------------------

void cSoftwareRenderer::blendPixel(int x, int y, float r, float g, float b, float a) {
  if (x < 0 || y < 0 || x >= int(fb_.width) || y >= int(fb_.height)) return;
  if (a <= 0.f) return;
  uint32_t& dst = fb_.pixels[size_t(y) * fb_.width + size_t(x)];
  if (a >= 1.f) {
    dst = 0xFF000000u | (uint32_t(std::lround(std::clamp(r, 0.f, 1.f) * 255.f)) << 16) |
          (uint32_t(std::lround(std::clamp(g, 0.f, 1.f) * 255.f)) << 8) |
          uint32_t(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
    return;
  }
  const float ia = 1.f - a;
  const float dr = float((dst >> 16) & 0xFF) / 255.f, dg = float((dst >> 8) & 0xFF) / 255.f,
              db = float(dst & 0xFF) / 255.f;
  dst = 0xFF000000u |
        (uint32_t(std::lround(std::clamp(r * a + dr * ia, 0.f, 1.f) * 255.f)) << 16) |
        (uint32_t(std::lround(std::clamp(g * a + dg * ia, 0.f, 1.f) * 255.f)) << 8) |
        uint32_t(std::lround(std::clamp(b * a + db * ia, 0.f, 1.f) * 255.f));
}

void cSoftwareRenderer::rasterizeHudTriangle(const HudVertex& A, const HudVertex& B,
                                            const HudVertex& C) {
  // 2026-10-25 (JD/Archie HUD diet): same coverage rule as before (all three barycentrics >= 0
  // at the pixel centre), but the edge signs are tested without per-pixel divides, the row's
  // covered x-range is skipped to directly, and flat-colour triangles (every HUD rect and
  // icon run) blend one precomputed colour per pixel.
  const float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
  if (std::abs(area) < 1e-9f) return;
  const int x0 = std::max(0, int(std::floor(std::min({A.x, B.x, C.x}))));
  const int x1 = std::min(int(fb_.width) - 1, int(std::ceil(std::max({A.x, B.x, C.x}))));
  const int y0 = std::max(0, int(std::floor(std::min({A.y, B.y, C.y}))));
  const int y1 = std::min(int(fb_.height) - 1, int(std::ceil(std::max({A.y, B.y, C.y}))));
  if (x0 > x1 || y0 > y1) return;
  auto edge = [](const HudVertex& a, const HudVertex& b, float x, float y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
  };
  const bool flat = A.r == B.r && A.r == C.r && A.g == B.g && A.g == C.g && A.b == B.b && A.b == C.b &&
                    A.a == B.a && A.a == C.a && A.u < 0.f && B.u < 0.f && C.u < 0.f;
  const float inv = 1.f / area;
  if (g_swProfOn) { g_hudFlat += flat; g_hudBox += uint64_t(x1 - x0 + 1) * uint64_t(y1 - y0 + 1); }
  if (flat) {
    const float a = A.a;
    if (a <= 0.f) return;
    const bool opaque = a >= 1.f;
    const uint32_t solid = 0xFF000000u | (uint32_t(std::lround(std::clamp(A.r, 0.f, 1.f) * 255.f)) << 16) |
                           (uint32_t(std::lround(std::clamp(A.g, 0.f, 1.f) * 255.f)) << 8) |
                           uint32_t(std::lround(std::clamp(A.b, 0.f, 1.f) * 255.f));
    const float ra = A.r * a, ga = A.g * a, ba = A.b * a, ia = 1.f - a;
    for (int y = y0; y <= y1; ++y) {
      const float py = y + 0.5f;
      uint32_t* row = fb_.pixels.data() + size_t(y) * fb_.width;
      for (int x = x0; x <= x1; ++x) {
        const float px = x + 0.5f;
        if (edge(B, C, px, py) * inv < 0.f || edge(C, A, px, py) * inv < 0.f || edge(A, B, px, py) * inv < 0.f) continue;
        uint32_t& dst = row[x];
        if (opaque) { dst = solid; continue; }
        const float dr = float((dst >> 16) & 0xFF) / 255.f, dg = float((dst >> 8) & 0xFF) / 255.f, db = float(dst & 0xFF) / 255.f;
        dst = 0xFF000000u | (uint32_t(std::lround(std::clamp(ra + dr * ia, 0.f, 1.f) * 255.f)) << 16) |
              (uint32_t(std::lround(std::clamp(ga + dg * ia, 0.f, 1.f) * 255.f)) << 8) |
              uint32_t(std::lround(std::clamp(ba + db * ia, 0.f, 1.f) * 255.f));
      }
    }
    return;
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = edge(B, C, px, py) * inv;
      const float w1 = edge(C, A, px, py) * inv;
      const float w2 = edge(A, B, px, py) * inv;
      if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
      const float u = w0 * A.u + w1 * B.u + w2 * C.u;
      const float v = w0 * A.v + w1 * B.v + w2 * C.v;
      const float r = w0 * A.r + w1 * B.r + w2 * C.r;
      const float g = w0 * A.g + w1 * B.g + w2 * C.g;
      const float b = w0 * A.b + w1 * B.b + w2 * C.b;
      float a = w0 * A.a + w1 * B.a + w2 * C.a;
      if (u >= 0.f) {  // glyph: font-atlas coverage multiplies alpha (hud.frag)
        if (!hudAtlasReady_) {
          hudAtlas_ = buildHudFontAtlas();
          hudAtlasReady_ = true;
        }
        const int tx = std::clamp(int(u * float(hudAtlas_.width)), 0, hudAtlas_.width - 1);
        const int ty = std::clamp(int(v * float(hudAtlas_.height)), 0, hudAtlas_.height - 1);
        a *= float(hudAtlas_.pixels[size_t(ty) * size_t(hudAtlas_.width) + size_t(tx)]) / 255.f;
      }
      blendPixel(x, y, r, g, b, a);
    }
  }
}

void cSoftwareRenderer::drawHud(const FrameParams& p) {
  if (p.hud.rects.empty() && p.hud.texts.empty() && p.hud.draws.empty()) return;
  const auto tb = g_swProfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  buildHudVertices(p.hud, hudVerts_);
  if (g_swProfOn) g_hudBuildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count();
  const HudVertex* v = hudVerts_.data();
  const size_t n = hudVerts_.size();
  for (size_t i = 0; i + 2 < n;) {
    // 2026-10-25 (HUD diet): a flat, whole-pixel, axis-aligned quad (quad()'s 6-vertex layout:
    // every UI rect and crisp-icon run) is filled as one rect span per row instead of two
    // edge-tested triangles. Also blends the shared diagonal once, not twice.
    if (i + 5 < n && v[i].u < 0.f && v[i + 2].u < 0.f && v[i].x == v[i + 3].x && v[i].y == v[i + 3].y &&
        v[i + 2].x == v[i + 4].x && v[i + 2].y == v[i + 4].y && v[i + 1].y == v[i].y && v[i + 1].x == v[i + 2].x &&
        v[i + 5].x == v[i].x && v[i + 5].y == v[i + 2].y && v[i].r == v[i + 2].r && v[i].g == v[i + 2].g &&
        v[i].b == v[i + 2].b && v[i].a == v[i + 2].a && v[i].x == std::floor(v[i].x) && v[i].y == std::floor(v[i].y) &&
        v[i + 2].x == std::floor(v[i + 2].x) && v[i + 2].y == std::floor(v[i + 2].y)) {
      const int rx0 = std::max(0, int(std::min(v[i].x, v[i + 2].x))), rx1 = std::min(int(fb_.width), int(std::max(v[i].x, v[i + 2].x)));
      const int ry0 = std::max(0, int(std::min(v[i].y, v[i + 2].y))), ry1 = std::min(int(fb_.height), int(std::max(v[i].y, v[i + 2].y)));
      fillHudRectSpan(v[i], rx0, ry0, rx1, ry1);
      i += 6;
      continue;
    }
    rasterizeHudTriangle(v[i], v[i + 1], v[i + 2]);
    i += 3;
  }
}

void cSoftwareRenderer::fillHudRectSpan(const HudVertex& c, int x0, int y0, int x1, int y1) {
  const float a = c.a;
  if (a <= 0.f || x0 >= x1 || y0 >= y1) return;
  if (g_swProfOn) { g_hudFlat += 2; g_hudBox += uint64_t(x1 - x0) * uint64_t(y1 - y0); }
  if (a >= 1.f) {
    const uint32_t solid = 0xFF000000u | (uint32_t(std::lround(std::clamp(c.r, 0.f, 1.f) * 255.f)) << 16) |
                           (uint32_t(std::lround(std::clamp(c.g, 0.f, 1.f) * 255.f)) << 8) |
                           uint32_t(std::lround(std::clamp(c.b, 0.f, 1.f) * 255.f));
    for (int y = y0; y < y1; ++y) std::fill_n(fb_.pixels.data() + size_t(y) * fb_.width + x0, x1 - x0, solid);
    return;
  }
  const float ia = 1.f - a, ca[3] = {c.r * a, c.g * a, c.b * a};
  if (uint64_t(x1 - x0) * uint64_t(y1 - y0) < 512) {  // small (icon runs): blendPixel's math directly
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) blendPixel(x, y, c.r, c.g, c.b, a);
    return;
  }
  // Large: same rounding as blendPixel, via per-channel 256-entry tables (dst channel -> result).
  uint8_t lut[3][256];
  for (int ch = 0; ch < 3; ++ch)
    for (int d = 0; d < 256; ++d) lut[ch][d] = uint8_t(std::lround(std::clamp(ca[ch] + float(d) / 255.f * ia, 0.f, 1.f) * 255.f));
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = fb_.pixels.data() + size_t(y) * fb_.width;
    for (int x = x0; x < x1; ++x) {
      const uint32_t d = row[x];
      row[x] = 0xFF000000u | uint32_t(lut[0][(d >> 16) & 0xFF]) << 16 | uint32_t(lut[1][(d >> 8) & 0xFF]) << 8 | lut[2][d & 0xFF];
    }
  }
}

// World-space line segment, depth-tested against depth_ (highlight outline:
// GL draws it with depth test on). Projected with the frame viewProj;
// segments crossing the near plane are clipped coarsely (endpoint w test —
// the outline is 12 short edges around one block, so a fully-inside test
// per endpoint with 32 subdivisions is ample).
void cSoftwareRenderer::drawWorldLine(const FrameParams& p, const Vec3& a, const Vec3& b,
                                     uint32_t color) {
  auto project = [&](const Vec3& v, float out[3]) {
    const float* m = p.viewProj.m;
    const float cx = m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12];
    const float cy = m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13];
    const float cz = m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14];
    const float cw = m[3]*v.x + m[7]*v.y + m[11]*v.z + m[15];
    if (cw <= 1e-6f) return false;
    const float inv = 1.f / cw;
    out[0] = (cx * inv * 0.5f + 0.5f) * float(fb_.width);
    out[1] = (1.f - (cy * inv * 0.5f + 0.5f)) * float(fb_.height);
    out[2] = cz * inv;
    return true;
  };
  constexpr int kSteps = 32;
  float prev[3];
  bool havePrev = project(a, prev);
  for (int i = 1; i <= kSteps; ++i) {
    const float t = float(i) / kSteps;
    const Vec3 v{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    float cur[3];
    const bool ok = project(v, cur);
    if (ok && havePrev) {
      const int x = int(std::lround(cur[0] - 0.5f)), y = int(std::lround(cur[1] - 0.5f));
      if (x >= 0 && y >= 0 && x < int(fb_.width) && y < int(fb_.height)) {
        const size_t idx = size_t(y) * fb_.width + size_t(x);
        if (cur[2] < depth_[idx]) fb_.pixels[idx] = color;
      }
    }
    if (ok) std::memcpy(prev, cur, sizeof prev);
    havePrev = ok;
  }
}

// NDC line segment, no depth test (crosshair: GL draws it on top). NDC y is
// up; the framebuffer is y-down.
void cSoftwareRenderer::drawNdcLine(const Vec3& a, const Vec3& b, uint32_t color) {
  constexpr int kSteps = 64;
  for (int i = 0; i <= kSteps; ++i) {
    const float t = float(i) / kSteps;
    const float nx = a.x + (b.x - a.x) * t, ny = a.y + (b.y - a.y) * t;
    const int x = int(std::lround((nx * 0.5f + 0.5f) * float(fb_.width) - 0.5f));
    const int y = int(std::lround((1.f - (ny * 0.5f + 0.5f)) * float(fb_.height) - 0.5f));
    if (x >= 0 && y >= 0 && x < int(fb_.width) && y < int(fb_.height))
      fb_.pixels[size_t(y) * fb_.width + size_t(x)] = color;
  }
}

void cSoftwareRenderer::drawOverlayLines(const FrameParams& p) {
  if (p.highlight) {
    const auto lines = blockOutlineLines(p.hx, p.hy, p.hz);
    for (size_t i = 0; i + 1 < lines.size(); i += 2)
      drawWorldLine(p, lines[i], lines[i + 1], 0xFF0D0D14u);  // GL line color (0.05,0.05,0.08)
  }
  if (p.crosshair && p.height > 0) {
    const auto lines = crosshairLines(p.width, p.height);
    for (size_t i = 0; i + 1 < lines.size(); i += 2)
      drawNdcLine(lines[i], lines[i + 1], 0xFFFFFFFFu);
  }
}

namespace {
struct SwProf {
  bool on = std::getenv("LL_SW_PROF") != nullptr;
  double acc[8] = {}; int frames = 0;
  std::chrono::steady_clock::time_point t;
  void mark(int i) { if (!on) return; auto n = std::chrono::steady_clock::now(); acc[i] += std::chrono::duration<double, std::milli>(n - t).count(); t = n; }
  void start() { if (on) t = std::chrono::steady_clock::now(); }
  void end() {
    if (!on || ++frames < 60) return;
    std::fprintf(stderr, "swprof tris=%llu scan=%llu cov=%llu shade=%llu /frame\n", (unsigned long long)(g_tris.exchange(0)/frames), (unsigned long long)(g_pxScan.exchange(0)/frames), (unsigned long long)(g_pxCov.exchange(0)/frames), (unsigned long long)(g_pxShade.exchange(0)/frames));
    {  // Cortana 2026-10-25 (pass sw2): Hi-Z rejected rows of total rows visited
      const unsigned long long hz = g_hizRows.exchange(0) / frames, rt = g_rowsTotal.exchange(0) / frames;
      std::fprintf(stderr, "swprof hiz_rows_skipped=%llu of %llu rows (%.1f%%) /frame\n", hz, rt, rt ? 100.0 * double(hz) / double(rt) : 0.0);
      const unsigned long long ps = g_pxSimd.exchange(0) / frames, pc = g_pxScalar.exchange(0) / frames;
      std::fprintf(stderr, "swprof span px simd(avx2/sse)=%llu scalar=%llu (simd %.1f%%) /frame\n", ps, pc, ps + pc ? 100.0 * double(ps) / double(ps + pc) : 0.0);
      std::fprintf(stderr, "swprof strip thread ms/frame min=%.2f max=%.2f\n", g_thrMinAcc / frames, g_thrMaxAcc / frames);
      g_thrMinAcc = g_thrMaxAcc = 0;
      std::fprintf(stderr, "swprof hud ms/frame=%.2f hud tris=%llu /frame\n", g_hudMsAcc / frames, (unsigned long long)(g_hudTris / frames));
      std::fprintf(stderr, "swprof hud build ms/frame=%.2f flat tris=%llu bbox px=%llu /frame\n", g_hudBuildMs / frames,
                   (unsigned long long)(g_hudFlat / frames), (unsigned long long)(g_hudBox / frames));
      g_hudMsAcc = 0; g_hudTris = 0; g_hudBuildMs = 0; g_hudFlat = 0; g_hudBox = 0;
    }
    std::fprintf(stderr, "swprof quad centrecull=%llu /frame\n", (unsigned long long)(g_quadCentreCulled.exchange(0) / frames));
    std::fprintf(stderr, "swprof centrecull tris=%llu emitmemo vertex hits=%llu /frame\n", (unsigned long long)(g_centreCulled.exchange(0)/frames), (unsigned long long)(g_emitMemoHits.exchange(0)/frames));  // Cortana 2026-10-25 box
    std::fprintf(stderr, "swprof facecull quads culled=%llu of %llu /frame\n", (unsigned long long)(g_fcCulled.exchange(0)/frames), (unsigned long long)(g_fcQuads.exchange(0)/frames));  // Pass 2 step 3
    std::fprintf(stderr, "swprof ms/frame: clear+cull=%.2f transform+bin=%.2f strips=%.2f pbr+ent+sprites=%.2f post+hud=%.2f\n",
                 acc[0]/frames, acc[1]/frames, acc[2]/frames, acc[3]/frames, acc[4]/frames);
    for (double& a : acc) a = 0; frames = 0;
  }
};
SwProf g_swProf;
}  // namespace

void cSoftwareRenderer::renderFrame(const FrameParams& p) {
  // Ginny (Argon) 2026-10-16 Pass 2 A/B proof (Cortana): LL_SW_AB=<n> renders frame n with
  // the LL_SW_AB_A mask (default 0 = pre-Pass-2), then again with the active mask, and logs
  // how many pixels differ. Off (zero cost beyond one branch) unless the env var is set.
  {
    static const int abAt = std::getenv("LL_SW_AB") ? std::atoi(std::getenv("LL_SW_AB")) : 0;
    static int abFrame = 0;
    static bool abIn = false;
    if (abAt > 0 && !abIn && ++abFrame == abAt) {
      abIn = true;
      const unsigned cur = g_p2.load();
      const char* am = std::getenv("LL_SW_AB_A");
      g_p2.store(am && *am ? unsigned(std::strtoul(am, nullptr, 0)) : 0u);
      renderFrame(p);
      const std::vector<uint32_t> a = fb_.pixels;
      g_p2.store(cur);
      renderFrame(p);
      size_t diff = 0; int maxd = 0;
      const size_t n = std::min(a.size(), fb_.pixels.size());
      for (size_t i = 0; i < n; ++i) if (a[i] != fb_.pixels[i]) {
        ++diff;
        for (int sh = 0; sh < 24; sh += 8) maxd = std::max(maxd, std::abs(int((a[i] >> sh) & 255) - int((fb_.pixels[i] >> sh) & 255)));
      }
      std::fprintf(stderr, "swab frame=%d maskA=0x%x maskB=0x%x pixels=%zu diff=%zu maxdelta=%d%s\n", abAt,
                   am && *am ? unsigned(std::strtoul(am, nullptr, 0)) : 0u, cur, n, diff, maxd, a.size() == fb_.pixels.size() ? "" : " SIZE-MISMATCH");
      std::fflush(stderr);
      logInfo("swab frame=%d maskB=0x%x pixels=%zu diff=%zu maxdelta=%d", abAt, cur, n, diff, maxd);
      abIn = false;
      return;
    }
  }
  g_swProf.start();
  if (p.width <= 0 || p.height <= 0) return;  // as the Vulkan backend
  // SSAA2x (JD 2026-10-13): the world stage rasterizes at 2x in both axes
  // and box-resolves to the target size before the overlay/HUD stage, so
  // HUD and crosshair stay at native resolution (never supersampled).
  const bool ssaa = aaMode_ == SoftwareAaMode::Ssaa2x;
  const uint32_t targetW = uint32_t(p.width), targetH = uint32_t(p.height);
  // Pass sw3 (--sw-scale): world stage at scale x target, nearest upscale before overlays.
  const float rscale = ssaa ? 1.0f : g_swRenderScale.load(std::memory_order_relaxed);
  const bool scaled = rscale < 0.999f;
  const uint32_t renderW = ssaa ? targetW * 2 : scaled ? std::max(1u, uint32_t(float(targetW) * rscale + 0.5f)) : targetW;
  const uint32_t renderH = ssaa ? targetH * 2 : scaled ? std::max(1u, uint32_t(float(targetH) * rscale + 0.5f)) : targetH;
  if (fb_.width != renderW || fb_.height != renderH) resize(renderW, renderH);
  {  // 2026-10-25 (Archie step 0): LL_SW_BLANK=1 skips the scene and HUD; the frame is a solid
     // colour cycling every frame, so present/blit cost can be measured alone. Off unless set.
    static const bool blank = std::getenv("LL_SW_BLANK") != nullptr;
    if (blank) {
      static uint32_t k = 0;
      ++k;
      if (fb_.width != targetW || fb_.height != targetH) resize(targetW, targetH);
      swsimd::fill32(fb_.pixels.data(), fb_.pixels.size(), 0xFF000000u | ((k * 5u) & 0xFF) << 16 | ((k * 3u) & 0xFF) << 8 | ((k * 7u) & 0xFF));
      g_swProf.end();
      return;
    }
  }
  // Ginny (Argon) 2026-10-16 Pass 2 step 2: with kP2Par the colour/depth clear moves into
  // the strip workers (each strip clears its own 32 rows right before drawing them: no
  // serial 7 MB fill on the calling thread, and the rows are hot in that core's cache).
  const bool p2par = swP2(kP2Par);
  {  // Pass 2: one-time report of the active feature mask + texture layout
    static bool reported = false;
    if (!reported && textures_.data()) {
      reported = true;
      logInfo("software renderer: Pass 2 mask 0x%x (LL_SW_P2), texture layout %s, %u layers", g_p2.load(),
              swP2(kP2Morton) && textures_.dataMorton() ? "morton 16x16" : "linear", textures_.layers());
    }
  }
  const uint32_t skyColor = swSkyColor(p.sky);
  if (p2par) hizReset(); else clear(skyColor);
  // Culling prototypes + frustum margin (M1): beginFrame arms the comet /
  // velocity state; decide() then culls per mesh exactly like GL/VK. All
  // prototypes default off, so decide() is the plain margin test.
  cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
  twinkleMode_ = p.leafTwinkle;  // 2026-10-16 crystal twinkle: render clock only
  shimmerTime_ = float(std::fmod(double(p.timeSec), 3600.0));  // Cortana 2026-10-16: render.shimmer clock
  twinklePhase_ = p.leafTwinkle == 2 ? uint32_t(int64_t(p.timeSec * 0.8f * 256.0f)) & 255u : 0u;
  stats_.meshes = uint32_t(meshes_.size() + pbrMeshes_.size());
  stats_.drawn = 0;
  // Stored chunk meshes at their sub-chunk origins (the same per-mesh
  // translation the MCP capture path bakes into its MVP), in key order.
  // Depth testing makes the order immaterial for the opaque world.
  // stats_.drawn counts only meshes actually submitted (post-cull), like
  // the GPU backends (fixes the L6 comparability gap).
  // RND-H4 (2026-10-15): cull first, then draw the visible meshes
  // front-to-back (nearest centre first) so the depth test rejects hidden
  // pixels before they are shaded. Ties keep key order (stable sort), so
  // the result is reproducible frame to frame.
  frameEmit_.clear();
  static const bool emitForce = [] { const char* e = std::getenv("LL_SW_EMIT"); return e && *e == '1'; }();  // Cortana 2026-10-25 box: test/profiling override, settings untouched
  if (emissiveLightOn_ || emitForce) {  // Cortana 2026-10-16 (render.emissive_light): 32 nearest emitters within 32 blocks
    // CREW TILE (shake 2026-10-18): member scratches, cleared per frame
    // (were fresh allocations every frame this block ran).
    std::vector<std::pair<float, size_t>>& cand = emitCandScratch_;
    cand.clear();
    std::vector<const float*>& src = emitSrcScratch_;
    src.clear();
    for (const auto& [k2, m2] : meshes_) {
      (void)k2;
      for (size_t i = 0; i + 5 < m2.emit.size(); i += 6) {
        const float dx = m2.emit[i] - p.camPos.x, dy = m2.emit[i + 1] - p.camPos.y, dz = m2.emit[i + 2] - p.camPos.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 32.f * 32.f) { cand.push_back({d2, src.size()}); src.push_back(&m2.emit[i]); }
      }
    }
    const size_t keep = std::min<size_t>(cand.size(), 32);
    std::partial_sort(cand.begin(), cand.begin() + keep, cand.end());
    for (size_t i = 0; i < keep; ++i) frameEmit_.insert(frameEmit_.end(), src[cand[i].second], src[cand[i].second] + 6);
  }
  drawOrder_.clear();
  for (auto& [key, m] : meshes_) {
    (void)key;
    const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
    if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos,
                          frustumMarginDeg_) != CullDecision::Full)
      continue;
    const float dx = mn.x + 8.f - p.camPos.x, dy = mn.y + 8.f - p.camPos.y, dz = mn.z + 8.f - p.camPos.z;
    drawOrder_.emplace_back(dx * dx + dy * dy + dz * dz, &m);
  }
  // Elfie 2026-10-17: LOD is now a draw range, not a swap (the mesh pointer sort
  // below makes per-index state fragile, so the range is derived where used).
  // Beyond the switch distance only the untagged prefix is active.
  auto drawVertsOf = [&p](const StoredMesh& m) -> size_t {
    if (m.hasLod) {  // Cortana 2026-10-16: per-sub-chunk LOD by horizontal centre distance
      const float lx = m.cx * 16.f + 8.f - p.camPos.x, lz = m.cz * 16.f + 8.f - p.camPos.z;
      if (lx * lx + lz * lz > kLodSwitchBlocks * kLodSwitchBlocks) return m.untaggedCount * 4;
    }
    return m.storedVerts();
  };
  std::stable_sort(drawOrder_.begin(), drawOrder_.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  // RND-H3 banding (2026-10-15): each band thread walks the same
  // front-to-back list but only writes its own rows, so the output is
  // identical to the serial pass (per-pixel depth test, same order per row).
  // Per-mesh screen row span from the 8 projected AABB corners, computed
  // once on the calling thread. Any corner at w <= 0 (camera inside or
  // behind) => conservatively the whole screen.
  meshRowY0_.resize(drawOrder_.size());
  meshRowY1_.resize(drawOrder_.size());
  auto meshRowSpan = [&](size_t di) {
    const StoredMesh& m = *drawOrder_[di].second;
    const float* M = p.viewProj.m;
    float ymin = 1e30f, ymax = -1e30f;
    bool full = false;
    for (int c = 0; c < 8 && !full; ++c) {
      const float x = m.cx * 16.f + ((c & 1) ? 16.f : 0.f);
      const float y = m.sy * 16.f + ((c & 2) ? 16.f : 0.f);
      const float z = m.cz * 16.f + ((c & 4) ? 16.f : 0.f);
      const float cy = M[1]*x + M[5]*y + M[9]*z + M[13];
      const float cw = M[3]*x + M[7]*y + M[11]*z + M[15];
      if (cw <= 1e-4f) { full = true; break; }
      const float sy = (1.0f - (cy / cw * 0.5f + 0.5f)) * float(fb_.height);
      ymin = std::min(ymin, sy); ymax = std::max(ymax, sy);
    }
    if (full) { meshRowY0_[di] = 0; meshRowY1_[di] = int(fb_.height); }
    else {
      meshRowY0_[di] = int(std::floor(std::max(-1.0f, ymin))) - 2;
      meshRowY1_[di] = int(std::ceil(std::min(float(fb_.height) + 1.0f, ymax))) + 2;
    }
  };
  if (!p2par) for (size_t di = 0; di < drawOrder_.size(); ++di) meshRowSpan(di);  // else: in phase 1
  g_swProf.mark(0);
  cSwBandPool& bandPool = cSwBandPool::get();
  // Cortana 2026-10-15 1112: transform every drawn mesh's verts ONCE (split
  // across the band threads by mesh), instead of once per band. Same
  // transformVertex math, so the screen positions are bit-identical.
  meshScreenOff_.resize(drawOrder_.size() + 1);
  meshScreenOff_[0] = 0;
  for (size_t di = 0; di < drawOrder_.size(); ++di)
    meshScreenOff_[di + 1] = meshScreenOff_[di] + drawOrder_[di].second->storedVerts() * 4;
  if (frameScreen_.size() < meshScreenOff_.back()) frameScreen_.resize(meshScreenOff_.back());
  // Elfie 2026-10-18 phase B: expansion scratch, indexed parallel to frameScreen_.
  meshVertOff_.resize(drawOrder_.size() + 1);
  meshVertOff_[0] = 0;
  for (size_t di = 0; di < drawOrder_.size(); ++di)
    meshVertOff_[di + 1] = meshVertOff_[di] + drawOrder_[di].second->storedVerts();
  if (frameVerts_.size() < meshVertOff_.back()) frameVerts_.resize(meshVertOff_.back());
  // Cortana 2026-10-25 (pass sw2, Archie): 16-row strips (were 32), pulled dynamically from
  // the atomic strip counter below; must equal kStripH. Hi-Z tiles (8 rows) never straddle.
  constexpr int kStripHF = 16;
  const int nStripsF = int((fb_.height + kStripHF - 1) / kStripHF);
  if (meshBins_.size() < drawOrder_.size()) { meshBins_.resize(drawOrder_.size()); meshBinOff_.resize(drawOrder_.size()); }
  if (binFill_.size() < 64) binFill_.resize(64);
  {
    // Ginny (Argon) 2026-10-16 Pass 2 step 2 (kP2Par): lockless two-phase setup. Phase 1
    // hands meshes out dynamically (one relaxed fetch_add per 4 meshes: big near meshes no
    // longer stall a static round-robin share); every mesh's strip bins + row span are
    // written by exactly one thread with thread-local fill cursors (no locks, no shared
    // writes). Phase 2 (strips) merges bins per strip in front-to-back mesh order, so the
    // draw order and therefore every pixel is unchanged.
    std::atomic<size_t> nextMesh{0};
    // Ginny (Argon) 2026-10-16 Pass 2 step 3 (kP2FaceCull): the eye point is recovered from
    // the projection itself (the null point of clip rows x, y, w), so the bucket test uses
    // exactly the camera the winding cull sees. A bucket whose limiting plane the eye is
    // behind (with a small margin) is back-facing as a whole: its quads are neither
    // transformed nor binned. Bucket 6 (non-axis or odd quads) is always kept.
    double eye[3] = {0, 0, 0};
    bool fcull = false;
    if (swP2(kP2FaceCull) && perPixel_) {
      const float* M = p.viewProj.m;
      const double a[3][4] = {{M[0], M[4], M[8], -double(M[12])}, {M[1], M[5], M[9], -double(M[13])}, {M[3], M[7], M[11], -double(M[15])}};
      auto det3 = [](double a0, double a1, double a2, double b0, double b1, double b2, double c0, double c1, double c2) {
        return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
      };
      const double D = det3(a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2]);
      if (std::fabs(D) > 1e-12) {
        for (int k = 0; k < 3; ++k) {
          double m3[3][3];
          for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m3[r][c] = c == k ? a[r][3] : a[r][c];
          eye[k] = det3(m3[0][0], m3[0][1], m3[0][2], m3[1][0], m3[1][1], m3[1][2], m3[2][0], m3[2][1], m3[2][2]) / D;
        }
        fcull = std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]);
      }
    }
    const swsimd::TransformQuadFn tq4 = swP2(kP2Dispatch) ? swsimd::transformQuadFn() : nullptr;
    bandPool.run([&](unsigned band) {
      const unsigned nb = bandPool.bands();  // read inside run: count may change between frames
      static thread_local std::vector<uint32_t> tlFill;
      size_t di = 0, chunkEnd = 0;
      bool first = true;
      for (;;) {
        if (p2par) {
          if (first || ++di >= chunkEnd) { di = nextMesh.fetch_add(4, std::memory_order_relaxed); chunkEnd = di + 4; }
        } else {
          di = first ? size_t(band) : di + nb;  // pre-Pass-2 static round-robin
        }
        first = false;
        if (di >= drawOrder_.size()) break;
        if (p2par) meshRowSpan(di);
        const StoredMesh& m = *drawOrder_[di].second;
        const size_t drawVerts = drawVertsOf(m);  // Elfie 2026-10-17: far LOD = untagged prefix
        const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
        const Mat4 mvp = mul(p.viewProj, swTranslation(mn.x, mn.y, mn.z));
        float* dst = frameScreen_.data() + meshScreenOff_[di];
        // Elfie 2026-10-18 phase B: expand this mesh's drawn range from the
        // packed store into the frame scratch; every transform/raster read
        // below consumes the scratch (byte-identical to the phase-A store).
        PackedVertex* mv = frameVerts_.data() + meshVertOff_[di];
        expandStored(m, 0, drawVerts, mv);
        // Pass 2 step 3: bucket visibility for this mesh (bit b = bucket b drawn).
        unsigned vis = 0x7Fu;
        const bool mcull = fcull && m.quadFace.size() == m.storedQuads();
        if (mcull) {
          constexpr double kMargin = 1.0 / 256.0;  // blocks; edge-on planes are never culled
          const double c[3] = {eye[0] - mn.x, eye[1] - mn.y, eye[2] - mn.z};
          for (int b = 0; b < 6; ++b) {
            const double lim = m.planeLim[b], e = c[b >> 1];
            const bool back = (b & 1) ? (e > lim + kMargin) : (e < lim - kMargin);
            if (back) vis &= ~(1u << b);
          }
        }
        // Pass 2 step 5 (kP2Dispatch): 4-wide SSE2/SSE4.1 quad transform (bit-identical).
        static_assert(sizeof(PackedVertex) % sizeof(uint16_t) == 0, "PackedVertex stride");
        static constexpr size_t kPvStride = sizeof(PackedVertex) / sizeof(uint16_t);
        const float fbW = float(fb_.width), fbH = float(fb_.height);
        auto xformQuad = [&](size_t q) {
          if (tq4) tq4(reinterpret_cast<const uint16_t*>(&mv[q * 4]), kPvStride, mvp.m, dst + q * 16, fbW, fbH);
          else for (size_t i = q * 4; i < q * 4 + 4; ++i) transformVertex(mv[i], mvp.m, dst + i * 4, fb_.width, fb_.height);
        };
        const size_t nq4 = drawVerts / 4;
        if (mcull && vis != 0x7Fu) {
          const uint8_t* qf = m.quadFace.data();
          for (size_t q = 0; q < nq4; ++q)
            if ((vis >> qf[q]) & 1u) xformQuad(q);
        } else {
          for (size_t q = 0; q < nq4; ++q) xformQuad(q);
          for (size_t i = nq4 * 4, n = drawVerts; i < n; ++i)
            transformVertex(mv[i], mvp.m, dst + i * 4, fb_.width, fb_.height);
        }
        const uint8_t* qface = (mcull && vis != 0x7Fu) ? m.quadFace.data() : nullptr;
        if (g_swProfOn) {
          uint64_t culled = 0;
          if (qface) for (size_t q = 0, nq = drawVerts / 4; q < nq; ++q) culled += ((vis >> qface[q]) & 1u) ? 0 : 1;
          g_fcCulled += culled; g_fcQuads += drawVerts / 4;
        }
        // Cortana 2026-10-15 1255: bin quads by 32-row strip (CSR per mesh)
        // so a strip visits only quads that can touch it, instead of every
        // quad of every mesh once per strip. Conservative (+-1 row); the
        // rasterizer's own band clamp stays the authority, so pixels are
        // unchanged. Quads with any w <= 0 (near-clip path) go to every strip.
        if (perPixel_) {
          const size_t nqd = drawVerts / 4;
          auto& off = meshBinOff_[di];
          auto& bins = meshBins_[di];
          off.assign(size_t(nStripsF) + 1, 0u);
          auto range = [&](size_t q, int& s0, int& s1) {
            const float* s = dst + q * 16;
            if (!(s[3] > 0 && s[7] > 0 && s[11] > 0 && s[15] > 0)) { s0 = 0; s1 = nStripsF - 1; return; }
            const float y0 = std::min(std::min(s[1], s[5]), std::min(s[9], s[13]));
            const float y1 = std::max(std::max(s[1], s[5]), std::max(s[9], s[13]));
            if (!(y1 >= -1.f) || !(y0 <= float(fb_.height) + 1.f)) { s0 = 1; s1 = 0; return; }
            s0 = std::max(0, int(std::floor(y0)) - 1) / kStripHF;
            s1 = std::min(int(fb_.height) - 1, int(std::ceil(y1)) + 1) / kStripHF;
          };
          for (size_t q = 0; q < nqd; ++q) { if (qface && !((vis >> qface[q]) & 1u)) continue; int s0, s1; range(q, s0, s1); for (int s = s0; s <= s1; ++s) ++off[size_t(s) + 1]; }
          for (int s = 0; s < nStripsF; ++s) off[size_t(s) + 1] += off[size_t(s)];
          bins.resize(off[size_t(nStripsF)]);
          auto& fill = p2par ? tlFill : binFill_[band];
          fill.assign(off.begin(), off.end() - 1);
          for (size_t q = 0; q < nqd; ++q) { if (qface && !((vis >> qface[q]) & 1u)) continue; int s0, s1; range(q, s0, s1); for (int s = s0; s <= s1; ++s) bins[fill[size_t(s)]++] = uint32_t(q); }
        }
      }
    });
  }
  g_swProf.mark(1);
  // Cortana 2026-10-15 1116: 32-row strips (power of two; colour+depth of a
  // 1280-wide strip is ~320 KB, inside one i5-12450H core's L2) handed out
  // dynamically, so the dense horizon strips don't leave threads idle.
  constexpr int kStripH = 16;  // pass sw2: = kStripHF (bins are per strip)
  const int nStrips = int((fb_.height + kStripH - 1) / kStripH);
  std::atomic<int> nextStrip{0};
  // 2026-10-25 (Archie step 1): strips are handed out heaviest-first (binned quad count,
  // longest-processing-time order), so the dense horizon strips start early and no thread is
  // left finishing one alone. Strips own disjoint rows, so pixels do not depend on the order.
  // A strip far heavier than a thread's fair share (the horizon rows) is split into 64-px-
  // multiple x tiles (Hi-Z tiles never straddle two), each its own work item.
  stripWork_.clear();
  {
    stripCost_.assign(size_t(nStrips), 1u);
    if (perPixel_)
      for (size_t di = 0; di < drawOrder_.size(); ++di) {
        const auto& off = meshBinOff_[di];
        if (off.size() < size_t(nStrips) + 1) continue;
        for (int s2 = 0; s2 < nStrips; ++s2) stripCost_[size_t(s2)] += off[size_t(s2) + 1] - off[size_t(s2)];
      }
    uint64_t total = 0;
    for (uint32_t c : stripCost_) total += c;
    const uint64_t share = std::max<uint64_t>(1, total / (uint64_t(std::max(1u, bandPool.bands())) * 3));
    const int maxTiles = std::max(1, int(fb_.width / 64));
    static const bool noSplit = std::getenv("LL_SW_NO_XTILES") != nullptr;
    for (int s2 = 0; s2 < nStrips; ++s2) {
      const uint64_t c = stripCost_[size_t(s2)];
      const int k = (perPixel_ && !noSplit) ? std::clamp(int((c + share - 1) / share), 1, maxTiles) : 1;
      const int tw = k == 1 ? int(fb_.width) : ((int(fb_.width) + k - 1) / k + 63) / 64 * 64;
      for (int x = 0; x < int(fb_.width); x += tw)
        stripWork_.push_back({s2, x, std::min(int(fb_.width), x + tw), uint32_t(c / uint64_t(k))});
    }
    std::stable_sort(stripWork_.begin(), stripWork_.end(), [](const StripWork& a, const StripWork& b) { return a.cost > b.cost; });
  }
  const int nWork = int(stripWork_.size());
  const auto drawBand = [&](unsigned bandIdx) {
   const auto tBand0 = g_swProfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
   struct BandTimer {  // pass sw2: per-thread strip time (LL_SW_PROF)
     unsigned i; std::chrono::steady_clock::time_point t0;
     ~BandTimer() { if (g_swProfOn && i < 64) g_bandMs[i] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
   } bandTimer{bandIdx, tBand0};
   for (int stripK; (stripK = nextStrip.fetch_add(1, std::memory_order_relaxed)) < nWork;) {
   const int strip = stripWork_[size_t(stripK)].strip;
   tlBandX0 = stripWork_[size_t(stripK)].x0;
   tlBandX1 = stripWork_[size_t(stripK)].x1 >= int(fb_.width) ? (1 << 30) : stripWork_[size_t(stripK)].x1;
   tlBandY0 = strip * kStripH;
   tlBandY1 = std::min(int(fb_.height), tlBandY0 + kStripH);
   if (p2par) {  // Pass 2 step 2: this strip's share of the frame clear
     const int cx0 = tlBandX0, cx1 = std::min(int(fb_.width), tlBandX1);
     for (int y = tlBandY0; y < tlBandY1; ++y) {  // this work item's rect only (x tiles)
       const size_t o = size_t(y) * fb_.width + size_t(cx0), n = size_t(cx1 - cx0);
       swsimd::fill32(fb_.pixels.data() + o, n, skyColor);
       swsimd::fillF(depth_.data() + o, n, 1.0f);
     }
   }
   // Cortana 2026-10-15 1755 [ENGINE]: pass 0 = opaque front-to-back (as before); pass 1 =
   // the strip's kShadeAlphaBlend quads far-to-near over meshes that have any (per-pixel
   // path only; meshes without blend quads keep the unfiltered pass-0 path).
   for (int pass = 0; pass < 2; ++pass) {
   for (size_t oi = 0; oi < drawOrder_.size(); ++oi) {
    const size_t di = pass == 0 ? oi : drawOrder_.size() - 1 - oi;
    if (pass == 1 && (!perPixel_ || !drawOrder_[di].second->hasBlend)) continue;
    if (meshRowY0_[di] >= tlBandY1 || meshRowY1_[di] < tlBandY0) continue;  // mesh misses this band
    const StoredMesh& m = *drawOrder_[di].second;
    const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
    const Mat4 mvp = mul(p.viewProj, swTranslation(mn.x, mn.y, mn.z));
    // Ground pulse + traces for this mesh (per-pixel path only; the flat
    // path has no fragment hook and stays as it was).
    groundFx_ = GroundFx{};
    for (int i = 0; i < 4; ++i) {
      groundFx_.pulseP0[i] = m.pulseP0[i];
      groundFx_.pulseP1[i] = m.pulseP1[i];
      groundFx_.traceP0[i] = m.traceP0[i];
      groundFx_.traceP1[i] = m.traceP1[i];
    }
    groundFx_.pulseTimeSec = p.groundPulseTimeSec;
    groundFx_.traceTimeSec = p.groundTraceTimeSec;
    groundFx_.pulseOn = p.groundPulseOn;
    groundFx_.traceOn = p.groundTraceOn;
    meshOriginX_ = mn.x; meshOriginY_ = mn.y; meshOriginZ_ = mn.z;
    if (perPixel_) {
      const auto& off = meshBinOff_[di];
      const uint32_t b0 = off[size_t(strip)], b1 = off[size_t(strip) + 1];
      if (b0 == b1) continue;
      tlQuadList = meshBins_[di].data() + b0;
      tlQuadCount = b1 - b0;
    }
    tlPreScreen = frameScreen_.data() + meshScreenOff_[di];
    tlCullBack = true;
    tlBlendPass = (perPixel_ && m.hasBlend) ? int8_t(pass) : int8_t(-1);
    renderMesh(frameVerts_.data() + meshVertOff_[di], drawVertsOf(m), mvp.m);  // Elfie 2026-10-17: far LOD prefix bound; 2026-10-18 phase B: frame scratch
    tlBlendPass = -1;
    tlCullBack = false;
    tlPreScreen = nullptr;
    tlQuadList = nullptr;
   }
   }
   }
   tlBandY0 = 0;
   tlBandY1 = 1 << 30;
   tlBandX0 = 0;
   tlBandX1 = 1 << 30;
   if (g_swProfOn) g_quadCentreCulled.fetch_add(tlQuadCentreCulled, std::memory_order_relaxed);
   tlQuadCentreCulled = 0;
   groundFx_ = GroundFx{};
   meshOriginX_ = meshOriginY_ = meshOriginZ_ = 0.f;
  };
  if (g_swProfOn) for (double& b : g_bandMs) b = 0;
  bandPool.run(drawBand);
  if (g_swProfOn) {
    const unsigned nb = std::min(64u, std::max(1u, bandPool.bands()));
    double mn = 1e30, mx = 0;
    for (unsigned b = 0; b < nb; ++b) { mn = std::min(mn, g_bandMs[b]); mx = std::max(mx, g_bandMs[b]); }
    g_thrMinAcc += mn; g_thrMaxAcc += mx;
  }
  g_swProf.mark(2);
  stats_.drawn += uint32_t(drawOrder_.size());
  groundFx_ = GroundFx{};
  meshOriginX_ = meshOriginY_ = meshOriginZ_ = 0.f;
  renderPbrMeshes(p);
  if (!entityVerts_.empty()) {
    // GAME-H1: entity verts are 1/16-block offsets from entityOrigin_.
    const Mat4 emvp = mul(p.viewProj, swTranslation(entityOrigin_[0], entityOrigin_[1], entityOrigin_[2]));
    meshOriginX_ = entityOrigin_[0]; meshOriginY_ = entityOrigin_[1]; meshOriginZ_ = entityOrigin_[2];
    renderEntityVerts(emvp.m);
    meshOriginX_ = meshOriginY_ = meshOriginZ_ = 0.f;
    ++stats_.drawn;
  }
  drawSprites(p);
  g_swProf.mark(3);
  // AA + ASCII stages (JD 2026-10-13): the world frame is finished here.
  // SSAA resolves down to target size; fxaa/cmaa post-filter in place;
  // ASCII (when on) then converts the AA'd frame to its glyph
  // visualization. Overlays/HUD draw after, at native resolution.
  if (scaled) {
    // Pass sw3: nearest-neighbour upscale of the scaled world frame to the target size,
    // split across the band threads by row. Depth resets like the SSAA path.
    aaOut_.resize(targetW, targetH);
    swScaleX_.resize(targetW);
    for (uint32_t x = 0; x < targetW; ++x) swScaleX_[x] = std::min(renderW - 1, uint32_t((uint64_t(x) * renderW) / targetW));
    const uint32_t* src = fb_.pixels.data();
    uint32_t* dstPx = aaOut_.pixels.data();
    const uint32_t* xm = swScaleX_.data();
    std::atomic<uint32_t> nextRow{0};
    bandPool.run([&](unsigned) {
      for (uint32_t y0; (y0 = nextRow.fetch_add(16, std::memory_order_relaxed)) < targetH;) {
        for (uint32_t y = y0, ye = std::min(targetH, y0 + 16); y < ye; ++y) {
          const uint32_t sy = std::min(renderH - 1, uint32_t((uint64_t(y) * renderH) / targetH));
          const uint32_t* s = src + size_t(sy) * renderW;
          uint32_t* d = dstPx + size_t(y) * targetW;
          if (y > 0 && sy == std::min(renderH - 1, uint32_t((uint64_t(y - 1) * renderH) / targetH)) && y != y0) {
            std::memcpy(d, d - targetW, size_t(targetW) * sizeof(uint32_t));  // repeated source row
          } else {
            for (uint32_t x = 0; x < targetW; ++x) d[x] = s[xm[x]];
          }
        }
      }
    });
    fb_.width = targetW;
    fb_.height = targetH;
    fb_.pixels = std::move(aaOut_.pixels);
    depth_.assign(size_t(targetW) * targetH, 1.0f);
    hizFbW_ = 0;
    applyAaPostPass();
  } else if (ssaa) {
    aaOut_.resize(targetW, targetH);
    resolveSsaa2x(fb_, aaOut_);
    fb_.width = targetW;
    fb_.height = targetH;
    fb_.pixels = std::move(aaOut_.pixels);
    depth_.assign(size_t(targetW) * targetH, 1.0f);  // overlay depth tests start fresh
    hizFbW_ = 0;  // Cortana 2026-10-15 1715: Hi-Z bounds no longer valid (depth went UP)
  } else {
    applyAaPostPass();
  }
  if (bloom_) applyBloomPost(fb_, (depth_.size() == fb_.pixels.size()) ? depth_.data() : nullptr, ensureAaScratch());  // Cortana 2026-10-16
  if (asciiEnabled_) applyAsciiPass();
  // Overlays (H3): highlight + crosshair lines, then the HUD layer — the
  // same order the GPU backends draw them (lines before HUD).
  drawOverlayLines(p);
  {  // 2026-10-25 (Archie bucket 5): HUD alone, split out of post+hud (LL_SW_PROF)
    const auto th = g_swProfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    drawHud(p);
    if (g_swProfOn) { g_hudMsAcc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - th).count(); g_hudTris += hudVerts_.size() / 3; }
  }
  if (grabArmed_) {
    grabArmed_ = false;
    grabW_ = int(fb_.width);
    grabH_ = int(fb_.height);
    grabRgb_.resize(size_t(grabW_) * size_t(grabH_) * 3);
    for (size_t i = 0, n = size_t(grabW_) * size_t(grabH_); i < n; ++i) {
      const uint32_t px = fb_.pixels[i];
      grabRgb_[3 * i] = uint8_t((px >> 16) & 0xFF);
      grabRgb_[3 * i + 1] = uint8_t((px >> 8) & 0xFF);
      grabRgb_[3 * i + 2] = uint8_t(px & 0xFF);
    }
    grabReady_ = true;
  }
  g_swProf.mark(4);
  g_swProf.end();
  // Debug aid (Cortana 2026-10-15 1340): LL_SW_SHOT=<file.ppm> and
  // LL_SW_SHOT_FRAME=<n> dump the finished frame n once (verification of
  // renderer changes on real game frames; off unless the env var is set).
  {
    static const char* shot = std::getenv("LL_SW_SHOT");
    static const int shotAt = std::getenv("LL_SW_SHOT_FRAME") ? std::atoi(std::getenv("LL_SW_SHOT_FRAME")) : 300;
    static int frameNo = 0;
    if (shot && ++frameNo == shotAt) {
      if (FILE* f = std::fopen(shot, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", fb_.width, fb_.height);
        for (uint32_t px : fb_.pixels) { const uint8_t rgb[3] = {uint8_t(px >> 16), uint8_t(px >> 8), uint8_t(px)}; std::fwrite(rgb, 1, 3, f); }
        std::fclose(f);
      }
    }
  }}

bool cSoftwareRenderer::grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
  if (!grabReady_) return false;
  rgb = grabRgb_;
  w = grabW_;
  h = grabH_;
  return true;
}

}  // namespace litholattice

// Cortana 2026-10-15 1610 (ENGINE): Debug Overlay status strip (see header).
namespace litholattice {
void cSoftwareRenderer::drawStatusStrip(uint32_t* px, int w, int h, const char* text) {
  if (!px || w <= 0 || h < stripRows() || !text) return;
  const int rows = stripRows();
  for (int y = 0; y < rows; ++y)
    for (int x = 0; x < w; ++x) {  // darken to ~25% (keeps the scene faintly visible)
      uint32_t& p = px[size_t(y) * size_t(w) + size_t(x)];
      p = 0xFF000000u | ((p >> 2) & 0x003F3F3Fu);
    }
  int x0 = 4;
  for (const char* t = text; *t && x0 + font::kCellW <= w; ++t, x0 += font::kCellW) {
    const int ci = int(static_cast<unsigned char>(*t)) - font::kFirst;
    if (ci <= 0 || ci >= font::kCount) continue;  // space / non-printable
    const char* g = font::kGlyphs[ci];
    for (int gy = 0; gy < font::kCellH; ++gy)
      for (int gx = 0; gx < font::kCellW; ++gx) {
        const char d = g[gy * font::kCellW + gx];
        const int cov = d >= 'a' ? d - 'a' + 10 : d >= 'A' ? d - 'A' + 10 : d - '0';
        if (cov <= 0) continue;
        uint32_t& p = px[size_t(gy + 1) * size_t(w) + size_t(x0 + gx)];
        const uint32_t v = uint32_t(cov * 17), bg = p & 0xFFu;
        const uint32_t l = (v * 255u + bg * (255u - v)) / 255u;
        p = 0xFF000000u | (l << 16) | (l << 8) | l;
      }
  }
}
}  // namespace litholattice
namespace litholattice { unsigned cSoftwareRenderer::autoRenderThreads(unsigned n) { return swAutoRenderThreads(n); } }  // Cortana 2026-10-15 1450
