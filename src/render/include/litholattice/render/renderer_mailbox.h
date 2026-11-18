// t46u item 2 (JD 2026-11-16): the renderer command mailbox. Replaces threeway::rendererMutex()
// and every RendererGuard call site (no mutex, no spin gate, no wait).
//
// Phase ownership:
//   * Not deferred (startup, single-thread path, after the render thread stops): the main
//     thread owns the backend and every call forwards straight through.
//   * Deferred (threeway split running): the render thread owns the backend. The game's
//     renderer calls on main become commands in an SPSC ring (producer: main, consumer: the
//     render thread). Main seals the ring when it submits a frame (P5); the render thread
//     applies commands up to that seal before it renders the frame. Reads come back through
//     triple-buffered snapshots the render thread publishes after each frame (stats, cull
//     stats, gpu ms, device lost, pool fill, texture array shape, the framebuffer grab).
// Mesh uploads keep the software backend's own SPSC upload ring (sw_upload_ring.h) when it has
// one; commands apply first at the seal, then the ring drains (same frame, same order the
// game issued them within each stream). Mesh uploads and clears never cross streams: one the
// upload ring refuses waits in a main-owned backlog and is retried at the next seal. Atomics only inside the two rings and the triple
// buffers' publish words.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "litholattice/assets/shader_library.h"
#include "litholattice/assets/texture_registry.h"
#include "litholattice/core/triple_buffer.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/texture_array.h"

namespace litholattice {

/// SPSC ring of renderer commands. Producer: main. Consumer: the render thread.
class cRenderCmdRing {
 public:
  using Cmd = std::function<void(IRenderer&)>;
  static constexpr uint64_t kSlots = 4096;  // power of two
  static constexpr uint64_t kMask = kSlots - 1;
  // producer
  bool tryPush(Cmd&& c) {
    const uint64_t t = tailShadow_;
    if (t - headCache_ >= kSlots) {
      headCache_ = head_.load(std::memory_order_acquire);
      if (t - headCache_ >= kSlots) return false;
    }
    slots_[t & kMask] = std::move(c);
    tailShadow_ = t + 1;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  uint64_t published() const { return tailShadow_; }  // producer's own view
  // consumer: run every command with index < target. Returns the number run.
  size_t applyUpTo(uint64_t target, IRenderer& r) {
    const uint64_t t = std::min(tail_.load(std::memory_order_acquire), target);
    size_t n = 0;
    for (uint64_t i = headShadow_; i < t; ++i, ++n) {
      Cmd c = std::move(slots_[i & kMask]);
      slots_[i & kMask] = nullptr;
      headShadow_ = i + 1;
      head_.store(i + 1, std::memory_order_release);
      if (c) c(r);
    }
    return n;
  }

 private:
  Cmd slots_[kSlots];
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) uint64_t tailShadow_ = 0, headCache_ = 0;  // producer-owned
  alignas(64) uint64_t headShadow_ = 0;                  // consumer-owned
};

class cRendererMailbox final : public IRenderer {
 public:
  explicit cRendererMailbox(IRenderer& r) : r_(r), tex_(*this) {}
  IRenderer& backend() { return r_; }

  // ---- phase control (main thread) ----
  /// On: the render thread owns the backend from now on (call before it starts).
  /// Off: the render thread has stopped; main applies whatever is still queued and owns it again.
  void setDeferred(bool on) {
    if (on == deferred_) return;
    if (on) {
      // Seed the read snapshot while main still owns the backend, so reads made before the
      // first deferred frame completes see the real state (texture shape, pool fill, ...).
      snapshotInto(read_.back());
      read_.publish();
    } else {
      flushSpill();
      ring_.applyUpTo(ring_.published(), r_);
      for (auto& c : spill_) c(r_);
      spill_.clear();
      for (auto& b : ringBacklog_) applyDirect(b);  // upload-ring items that never fit, in order
      ringBacklog_.clear();
    }
    deferred_ = on;
  }
  bool deferred() const { return deferred_; }
  /// P5 frame seal (main): flush the spill into the ring and return the command index the
  /// next frame must apply up to.
  uint64_t seal() { flushSpill(); return ring_.published(); }
  uint64_t commandsQueued() const { return queued_; }
  uint64_t commandsSpilled() const { return spilled_; }
  uint64_t ringBacklogged() const { return backlogged_; }

  // ---- render thread ----
  void applyUpTo(uint64_t seal) { ring_.applyUpTo(seal, r_); }
  /// After the frame: grab if armed, then publish the read snapshot.
  void afterFrame() {
    if (grabArmedRender_) {
      grabArmedRender_ = false;
      Grab& g = grab_.back();
      g.ok = r_.grabFramebuffer(g.rgb, g.w, g.h);
      g.seq = ++grabSeqRender_;
      grab_.publish();
    }
    snapshotInto(read_.back());
    read_.publish();
  }

  // ---- IRenderer ----
  const char* name() const override { return r_.name(); }
  std::string deviceInfo() const override { return r_.deviceInfo(); }
  std::string presentMode() const override { return r_.presentMode(); }
  ITextureArray& textureArray() override { return deferred_ ? static_cast<ITextureArray&>(tex_) : r_.textureArray(); }
  void setMaterials(const BlockMaterials& m) override {
    if (!deferred_) { r_.setMaterials(m); return; }
    post([c = std::make_shared<BlockMaterials>(m)](IRenderer& r) { r.setMaterials(*c); });
  }
  int loadShaders(const ShaderLibrary& s) override {
    if (!deferred_) return lastShaders_ = r_.loadShaders(s);
    post([this, c = std::make_shared<ShaderLibrary>(s)](IRenderer& r) { r.loadShaders(*c); });
    return lastShaders_;  // the count of the last direct load (the deferred result is logged by the backend)
  }
  void uploadChunkMesh(const ChunkMesh& mesh) override {
    if (!deferred_) { r_.uploadChunkMesh(mesh); return; }
    if (r_.meshUploadsQueued()) { toRing(RingItem{RingItem::Chunk, mesh, {}}); return; }  // keeps ring order
    post([c = mesh](IRenderer& r) { r.uploadChunkMesh(c); });
  }
  bool enqueueChunkMesh(ChunkMesh&& mesh) override {
    if (!deferred_) return r_.enqueueChunkMesh(std::move(mesh));
    if (r_.meshUploadsQueued()) return canUseRing() && r_.enqueueChunkMesh(std::move(mesh));
    post([c = std::move(mesh)](IRenderer& r) { r.uploadChunkMesh(c); });
    return true;
  }
  bool enqueuePbrChunkMesh(PbrChunkMesh&& mesh) override {
    if (!deferred_) return r_.enqueuePbrChunkMesh(std::move(mesh));
    if (r_.meshUploadsQueued()) return canUseRing() && r_.enqueuePbrChunkMesh(std::move(mesh));
    post([c = std::move(mesh)](IRenderer& r) { r.uploadPbrChunkMesh(c); });
    return true;
  }
  bool enqueueClearChunkMeshes() override {
    if (!deferred_) return r_.enqueueClearChunkMeshes();
    if (r_.meshUploadsQueued()) {
      // A clear is never refused: if the ring is full it waits in the ring backlog and every
      // later ring push is refused (the game keeps those meshes) until it lands, so order holds.
      toRing(RingItem{RingItem::Clear, {}, {}});
      return true;
    }
    post([](IRenderer& r) { r.clearChunkMeshes(); });
    return true;
  }
  void enqueuePbrMaterials(const cMaterialReg& reg) override {
    if (!deferred_ || r_.meshUploadsQueued()) { r_.enqueuePbrMaterials(reg); return; }  // ring backend: announce only
    post([p = &reg](IRenderer& r) { r.setPbrMaterials(*p); });
  }
  bool meshUploadsQueued() const override { return deferred_ || r_.meshUploadsQueued(); }
  float meshPoolFill() const override { return deferred_ ? rs().poolFill : r_.meshPoolFill(); }
  bool prepareChunkMeshAsync(const ChunkMesh& mesh, PreparedChunkMesh& out) override { return r_.prepareChunkMeshAsync(mesh, out); }
  void publishChunkMesh(const PreparedChunkMesh& p) override {
    if (!deferred_) { r_.publishChunkMesh(p); return; }
    post([c = p](IRenderer& r) { r.publishChunkMesh(c); });
  }
  void discardChunkMesh(const PreparedChunkMesh& p) override {
    if (!deferred_) { r_.discardChunkMesh(p); return; }
    post([c = p](IRenderer& r) { r.discardChunkMesh(c); });
  }
  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override {
    if (!deferred_) { r_.uploadEntityMesh(verts); return; }
    post([c = verts](IRenderer& r) { r.uploadEntityMesh(c); });
  }
  void setEntityOrigin(float x, float y, float z) override { fwd([=](IRenderer& r) { r.setEntityOrigin(x, y, z); }); }
  void clearChunkMeshes() override {
    if (!deferred_) { r_.clearChunkMeshes(); return; }
    enqueueClearChunkMeshes();
  }
  bool deviceLost() const override { return deferred_ ? rs().lost : r_.deviceLost(); }
  float gpuFrameMs() const override { return deferred_ ? rs().gpuMs : r_.gpuFrameMs(); }
  bool supportsPbrPath() const override { return r_.supportsPbrPath(); }
  bool supportsWaferReveal() const override { return r_.supportsWaferReveal(); }
  void setWaferReveal(const WaferRevealFrame& f) override {
    if (!deferred_) { r_.setWaferReveal(f); return; }
    post([c = f](IRenderer& r) { r.setWaferReveal(c); });
  }
  bool prefersGreedyMeshes() const override { return r_.prefersGreedyMeshes(); }
  void setPbrMaterials(const cMaterialReg& reg) override { fwd([p = &reg](IRenderer& r) { r.setPbrMaterials(*p); }); }
  void setPbrTier(PbrTransparencyTier t) override { fwd([=](IRenderer& r) { r.setPbrTier(t); }); }
  void setAmbientMode(AmbientMode m) override { fwd([=](IRenderer& r) { r.setAmbientMode(m); }); }
  void setFrustumMarginDeg(float d) override { fwd([=](IRenderer& r) { r.setFrustumMarginDeg(d); }); }
  void setModelDither(bool on, float n, float f) override { fwd([=](IRenderer& r) { r.setModelDither(on, n, f); }); }
  void setAaMode(const char* m) override { fwd([s = std::string(m ? m : "")](IRenderer& r) { r.setAaMode(s.c_str()); }); }
  void setBloom(bool on) override { fwd([=](IRenderer& r) { r.setBloom(on); }); }
  void setShimmer(bool on) override { fwd([=](IRenderer& r) { r.setShimmer(on); }); }
  void setMipmaps(bool on) override { fwd([=](IRenderer& r) { r.setMipmaps(on); }); }
  void setTextureFiltering(int f, int a) override { fwd([=](IRenderer& r) { r.setTextureFiltering(f, a); }); }
  int maxAnisotropy() const override { return r_.maxAnisotropy(); }
  void setEmissiveLight(bool on) override { fwd([=](IRenderer& r) { r.setEmissiveLight(on); }); }
  void setAo(bool on) override { fwd([=](IRenderer& r) { r.setAo(on); }); }
  void setTexRes(int t) override { fwd([=](IRenderer& r) { r.setTexRes(t); }); }
  void setAsciiMode(bool on, int cols) override { fwd([=](IRenderer& r) { r.setAsciiMode(on, cols); }); }
  void setAsciiGlyphSet(const char* s) override { fwd([n = std::string(s ? s : "")](IRenderer& r) { r.setAsciiGlyphSet(n.c_str()); }); }
  void setAsciiValueSpan(int s) override { fwd([=](IRenderer& r) { r.setAsciiValueSpan(s); }); }
  void setCullProtoConfig(const CullProtoConfig& c) override { fwd([=](IRenderer& r) { r.setCullProtoConfig(c); }); }
  CullProtoStats cullProtoStats() const override { return deferred_ ? rs().cull : r_.cullProtoStats(); }
  void uploadPbrChunkMesh(const PbrChunkMesh& mesh) override {
    if (!deferred_) { r_.uploadPbrChunkMesh(mesh); return; }
    if (r_.meshUploadsQueued()) { toRing(RingItem{RingItem::Pbr, {}, mesh}); return; }  // keeps ring order
    post([c = mesh](IRenderer& r) { r.uploadPbrChunkMesh(c); });
  }
  /// Only the owner of the backend draws: main when not deferred. A deferred call is a bug.
  void renderFrame(const FrameParams& p) override { if (!deferred_) r_.renderFrame(p); }
  RenderStats stats() const override { return deferred_ ? rs().stats : r_.stats(); }
  void requestGrab() override {
    if (!deferred_) { r_.requestGrab(); return; }
    post([this](IRenderer& r) { grabArmedRender_ = true; r.requestGrab(); });
  }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (!deferred_) return r_.grabFramebuffer(rgb, w, h);
    grab_.take();
    const Grab& g = grab_.front();
    if (g.seq == grabSeqTaken_ || !g.ok) return false;
    grabSeqTaken_ = g.seq;
    rgb = g.rgb; w = g.w; h = g.h;
    return true;
  }

 private:
  struct ReadState {
    RenderStats stats{};
    CullProtoStats cull{};
    float gpuMs = -1.f;
    bool lost = false;
    float poolFill = -1.f;
    int texLayers = 0, texTile = 0;
  };
  /// A must-land upload-ring item the ring had no room for (main-owned backlog). Commands
  /// apply before the upload ring at the seal, so a refused mesh must never fall back to the
  /// command ring (it would land ahead of older ring entries for the same column).
  struct RingItem {
    enum Kind : uint8_t { Clear, Chunk, Pbr } kind;
    ChunkMesh chunk;
    PbrChunkMesh pbr;
  };
  struct Grab {
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    bool ok = false;
    uint64_t seq = 0;
  };
  /// Deferred texture array: upload() queues a copy; the shape reads the last snapshot.
  class TexProxy final : public ITextureArray {
   public:
    explicit TexProxy(cRendererMailbox& m) : m_(m) {}
    bool upload(const cTextureReg& t) override {
      m_.post([c = std::make_shared<cTextureReg>(t)](IRenderer& r) { r.textureArray().upload(*c); });
      return true;
    }
    int layerCount() const override { return m_.rs().texLayers; }
    int tileSize() const override { return m_.rs().texTile; }
   private:
    cRendererMailbox& m_;
  };

  template <class F>
  void fwd(F&& f) {
    if (!deferred_) { f(r_); return; }
    post(std::forward<F>(f));
  }
  void post(cRenderCmdRing::Cmd c) {
    ++queued_;
    if (!spill_.empty() || !ring_.tryPush(std::move(c))) { if (c) { spill_.push_back(std::move(c)); ++spilled_; } }
  }
  void flushSpill() {
    while (!spill_.empty() && ring_.tryPush(std::move(spill_.front()))) spill_.pop_front();
    while (!ringBacklog_.empty() && tryRing(ringBacklog_.front())) ringBacklog_.pop_front();
  }
  bool canUseRing() {
    if (!ringBacklog_.empty()) flushSpill();
    return ringBacklog_.empty();
  }
  bool tryRing(RingItem& b) {  // the enqueue calls leave the mesh untouched when they refuse
    switch (b.kind) {
      case RingItem::Clear: return r_.enqueueClearChunkMeshes();
      case RingItem::Chunk: return r_.enqueueChunkMesh(std::move(b.chunk));
      case RingItem::Pbr: return r_.enqueuePbrChunkMesh(std::move(b.pbr));
    }
    return true;
  }
  void applyDirect(RingItem& b) {
    switch (b.kind) {
      case RingItem::Clear: r_.clearChunkMeshes(); break;
      case RingItem::Chunk: r_.uploadChunkMesh(b.chunk); break;
      case RingItem::Pbr: r_.uploadPbrChunkMesh(b.pbr); break;
    }
  }
  void toRing(RingItem&& b) {
    if (canUseRing() && tryRing(b)) return;
    ringBacklog_.push_back(std::move(b));
    ++backlogged_;
  }
  void snapshotInto(ReadState& s) {  // the backend's owner only
    s.stats = r_.stats();
    s.cull = r_.cullProtoStats();
    s.gpuMs = r_.gpuFrameMs();
    s.lost = r_.deviceLost();
    s.poolFill = r_.meshPoolFill();
    s.texLayers = r_.textureArray().layerCount();
    s.texTile = r_.textureArray().tileSize();
  }
  const ReadState& rs() const { read_.take(); return read_.front(); }

  IRenderer& r_;
  TexProxy tex_;
  bool deferred_ = false;  // main-owned; flipped only while no render thread runs
  cRenderCmdRing ring_;
  std::deque<cRenderCmdRing::Cmd> spill_;  // main-owned overflow, kept in order
  std::deque<RingItem> ringBacklog_;       // main-owned, kept in order
  uint64_t backlogged_ = 0;
  int lastShaders_ = 0;
  uint64_t queued_ = 0, spilled_ = 0;
  mutable cTripleBuffer<ReadState> read_;  // render thread writes, main reads
  cTripleBuffer<Grab> grab_;
  bool grabArmedRender_ = false;  // render-thread owned (set by the queued requestGrab)
  uint64_t grabSeqRender_ = 0;    // render-thread owned
  uint64_t grabSeqTaken_ = 0;     // main-owned
};

}  // namespace litholattice