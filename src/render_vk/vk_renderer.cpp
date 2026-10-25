// Vulkan backend for IRenderer: one graphics queue, kFramesInFlight frames in flight (fence +
// acquire semaphore per frame, render-done semaphore per swapchain image), swapchain rebuilt on
// resize / out-of-date. Chunk vertices live in a cVertexArena; replaced meshes are freed once no
// frame in flight can still read them. Main/render thread only.
#include "litholattice/core/tick_profile.h"  // LL_TICK_PROFILE (measure only; Proposed by JD; co-implemented by JD and Cortana)
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include "vk_device.h"
#include "vk_frame_uniforms.h"
#include "vk_pipelines.h"
#include "vk_resources.h"
#include "vk_swapchain.h"
#include "vk_texture_array.h"
#include "vk_vertex_arena.h"
#include "litholattice/render_vk/packed_vertex12.h"  // 0.0.1.59 [P59-1]
#include "litholattice/assets/shader_library.h"
#include "litholattice/core/log.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render_vk/sprite_vertices.h"
#include "litholattice/render_vk/vulkan_renderer.h"
#include <cassert>
#include <string>  // 0.0.1.59-b F59-1
#include <cctype>  // 0.0.1.59-b F59-1
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <thread>  // 0.0.1.56 F56-8
#include "litholattice/render_vk/packed_quad64.h"  // Mega-Pipeline Phase 2
// Phase 4.0 (Direction JD, research Elfie, implementation Cortana): streaming / culling counters. Logging only,
// zero behavior change. CMake option LITHO_STREAM_COUNTERS (default ON) sets this; OFF compiles them out.
#ifndef LL_STREAM_COUNTERS
#define LL_STREAM_COUNTERS 1
#endif

namespace litholattice {
namespace {

using namespace vk;

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxQuads = kMaxQuadsPerMesh;  // shared quad index buffer size (renderer.h)
constexpr VkDeviceSize kOverlayBytes = 4 << 20;  // per frame: lines + HUD vertices (~130k: a full-screen editor)
// 0.0.1.52 [A52-4] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): 32 MB per slot (was 8). A full ring no longer
// flushes with a queue-wait-idle submit: it grows by a temporary overflow buffer that dies with the
// slot's fence.
constexpr VkDeviceSize kStagingBytes = VkDeviceSize(32) << 20;  // per frame slot (device-local vertex uploads)
constexpr VkDeviceSize kHudOffset = 512;         // HUD vertices start here (lines before)

static_assert(sizeof(vk_sprite::SpriteVertex) == 40, "sprite vertex layout (vk_pipelines attributes)");

/// Linear, clamp-to-edge sampler for the sprite atlas pages (GL33 uses
/// GL_LINEAR for its sprite page textures; the block texture array's
/// nearest-mag filtering is the chunk path's choice, not the sprites').
UniqueSampler createLinearSampler(const cVulkanDevice& dev) {
  auto si = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler s = VK_NULL_HANDLE;
  check(vkCreateSampler(dev.device(), &si, nullptr, &s), "vkCreateSampler (sprite atlas)");
  return UniqueSampler(dev.device(), s);
}

int64_t meshKey(int cx, int sy, int cz) {
  // Fix 2026-10-11 (audit): left-shifting negative int64_t is UB; cast through
  // uint64_t first, then back (bit-preserving).
  return int64_t((uint64_t(uint32_t(cx)) << 40) ^ (uint64_t(uint32_t(sy)) << 32) ^ uint64_t(uint32_t(cz)));
}

class cVulkanRenderer final : public IRenderer {
 public:
  // Ginny 2026-10-16 (perf list): greedy-merged chunk faces; chunk_shading.glsl repeats the tile.
  bool prefersGreedyMeshes() const override { return true; }
  ~cVulkanRenderer() override {
    if (dev_) vkDeviceWaitIdle(dev_->device());
    if (dev_) for (FrameSlot& f : frames_) if (f.timer) vkDestroyQueryPool(dev_->device(), f.timer, nullptr);  // [A52-3]
  }

  bool init(const VulkanRendererDesc& desc, std::string* error) {
    dev_ = cVulkanDevice::create(desc, error);
    if (!dev_) return false;
    const VkDevice d = dev_->device();
    auto pi = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = dev_->queueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateCommandPool(d, &pi, nullptr, &pool), "vkCreateCommandPool")) return fail(error, "command pool");
    pool_ = UniqueCommandPool(d, pool);

    const auto surface = chooseSurfaceConfig(*dev_, desc.vsync, desc.presentMode);
    if (!surface) return fail(error, "no usable surface format / depth format");
    surface_ = *surface;
    surface_.wantImages = uint32_t(std::clamp(desc.swapImages, 0, 8));
    logInfo("vulkan: buffering requested %d swapchain images (0 = auto)", desc.swapImages);
    pipelines_ = RenderPipelines::create(*dev_, surface_.color.format, surface_.depth, error, packed12_);
    // Mega-Pipeline Phase 2 (Direction JD, design Archie (Mega-Pipeline brief), implementation Cortana): LL_VK_PULL=1
    // turns on vertex pulling (64-bit packed quads). Needs V2b, Packed12, firstInstance, dynamic storage-array indexing.
    // Phase 4.2a (Direction JD, LithoPipeline designed by Archie, implemented by Cortana): LL_VK_FASTCULL=1.
    if (const char* e = std::getenv("LL_VK_FASTCULL"); e && std::atoi(e) == 1) { fastCull_ = true; logInfo("vulkan: Phase 4.2a compact cull list on"); }
    if (const char* e = std::getenv("LL_VK_PULL"); e && std::atoi(e) == 1) {
      pull_ = pipelines_ && pipelines_->chunkPull() && packed12_ && megaMode_ == 3 && dev_->drawIndirectFirstInstance() &&
              dev_->storageArrayDynamicIndexing();
      logInfo("vulkan: Mega-Pipeline Phase 2 vertex pulling %s", pull_ ? "on" : "requested but unavailable (needs LL_VK_MEGA=3, packed12, firstInstance, dynamic storage indexing); off");
    }
    // Mega-Pipeline Phase 3: LL_VK_BUCKETS=1 (with pulling) draws only the face buckets that can face the camera.
    // Direction JD, design Archie (Mega-Pipeline brief), implementation Cortana. Buckets are the face-sorted ranges
    // built in buildPulled; adjacent visible buckets merge into one indirect command (all in the shared arena, MDI).
    if (const char* e = std::getenv("LL_VK_BUCKETS"); pull_ && e && std::atoi(e) == 1) buckets_ = true;
    if (const char* e = std::getenv("LL_VK_BLENDSPLIT"); e && std::atoi(e) == 0) blendSplit_ = false;  // Phase 4 B
    if (pull_) logInfo("vulkan: Phase 4 blend split %s", blendSplit_ ? "on" : "off (LL_VK_BLENDSPLIT=0)");
    if (buckets_) logInfo("vulkan: Mega-Pipeline Phase 3 camera-facing face buckets on");
    if (!pipelines_) return false;
    cVertexArena::Kind kind = cVertexArena::Kind::Device;
    if (!cVertexArena::parseKind(desc.vertexArena, kind)) logError("vulkan: unknown vertex arena '%s'; using device", desc.vertexArena.c_str());
    {  // 0.0.1.52 [A52-4] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): auto-ReBAR. A DEVICE_LOCAL|HOST_VISIBLE heap
      // larger than the classic 256 MB BAR window means Resizable BAR / SAM: vertex blocks are then
      // written in place (no staging, no copy). Exactly 256 MB is the legacy window and stays unused.
      // An explicit vertex arena choice (settings / CLI) other than "device" is respected.
      VkPhysicalDeviceMemoryProperties mp{};
      vkGetPhysicalDeviceMemoryProperties(dev_->physical(), &mp);
      VkDeviceSize bar = 0;
      constexpr auto kBar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((mp.memoryTypes[i].propertyFlags & kBar) == kBar) bar = std::max(bar, mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size);
      const bool rebar = bar > (VkDeviceSize(256) << 20);
      logInfo("vulkan: DEVICE_LOCAL|HOST_VISIBLE heap %llu MB -> Resizable BAR %s", (unsigned long long)(bar >> 20), rebar ? "yes" : "no");
      if (rebar && kind == cVertexArena::Kind::Device && std::getenv("LL_VK_NO_REBAR") == nullptr) {
        kind = cVertexArena::Kind::ReBar;
        logInfo("vulkan: vertex arena auto-upgraded to rebar");
      }
    }
    arena_ = std::make_unique<cVertexArena>(*dev_, kind, VkDeviceSize(32) << 20, packed12_ ? VkDeviceSize(192) : VkDeviceSize(64));
    // 0.0.1.59 [P59-1] Packed12 (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the
    // Bitmaster and the Bitmasker; layout correction and build by Cortana).
    logInfo("vulkan: chunk vertex format %s (%u bytes, arena alignment %llu; LL_VK_PACKED12=%d to switch)",
            packed12_ ? "packed12" : "packed16", unsigned(vertexStride_), (unsigned long long)arena_->alignment(), packed12_ ? 0 : 1);
    timestampPeriodNs_ = dev_->properties().limits.timestampPeriod;  // 0.0.1.52 [A52-3]
    logInfo("vulkan: vertex arena %s, quad index buffer %s", cVertexArena::kindName(kind), kind == cVertexArena::Kind::Host ? "host-visible" : "device-local");
    if (!createStaticBuffers() || !createFrames() || !createDescriptors()) return fail(error, "frame resources");
    textures_ = std::make_unique<cVkTextureArray>(*dev_, pool_.get(), [this] { writeDescriptors(); });
    if (!textures_->upload(cTextureReg{})) return fail(error, "initial texture upload");
    logInfo("vulkan: using %s", dev_->description().c_str());
    logInfo("vulkan: mega-draw mode %d (LL_VK_MEGA; multiDrawIndirect %s, drawIndirectFirstInstance %s)", megaMode_,
            dev_->multiDrawIndirect() ? "yes" : "no", dev_->drawIndirectFirstInstance() ? "yes" : "no");
    return true;
  }

  const char* name() const override { return "Vulkan"; }
  std::string presentMode() const override { return presentModeName(surface_.presentMode); }
  std::string deviceInfo() const override { return dev_->description(); }
  ITextureArray& textureArray() override { return *textures_; }
  // PROVISIONAL (JD 2026-10-24): texture-filtering stand-in.
  void setTextureFiltering(int filter, int aniso) override { if (textures_) textures_->setFiltering(filter, aniso); }
  int maxAnisotropy() const override { return dev_ ? int(dev_->maxAnisotropy()) : 1; }

  void setMaterials(const BlockMaterials& m) override {
    const MaterialBindings& b = m.bindings();
    FrameUniforms& u = materials_;
    for (size_t c = 0; c < 2; ++c) { u.stripPalette[c].value = b.stripPalette[c]; u.stripRing[c].value = b.stripRing[c]; }
    for (size_t t = 0; t < 3; ++t) {
      u.tierPalette[t].value = b.tierPalette[t];
      u.tierMask[t].value = b.tierMask[t];
      u.tierRim[t].value = b.tierRim[t];
    }
    u.pipMask = b.pipMask;
    u.cellMask = b.cellMask;
    for (size_t c = 0; c < 2; ++c) u.stripDot[c].value = b.stripDot[c];
    u.digits = b.debugDigits;
    for (size_t k = 0; k < 4; ++k) u.leafGlow[k].value = b.leafGlow[k];  // 2026-10-16 crystal twinkle
    for (size_t k = 0; k < 32; ++k) u.tintMask[k].value = b.tintMask[k];  // 2026-10-16 kShadeTraceTint
    for (size_t k = 0; k < std::size(u.fxMap); ++k) u.fxMap[k] = m.fxMap(uint32_t(k));  // JD Vulkan push
    const auto anims = m.animations();
    for (size_t i = 0; i < std::size(u.animStart); ++i) {
      const ShaderAnimation a = i < anims.size() ? anims[i] : ShaderAnimation{};
      u.animStart[i].value = a.sequenceStart;
      u.animLength[i].value = a.sequenceLength;
      u.animFrameMs[i].value = a.frameMs;
    }
    const auto frames = m.animationFrames();
    for (size_t i = 0; i < std::size(u.animSequence); ++i) u.animSequence[i].value = i < frames.size() ? frames[i] : 0;
  }

  // Vulkan shaders are SPIR-V built in at compile time; pack shader files are OpenGL GLSL.
  int loadShaders(const ShaderLibrary& lib) override {
    if (lib.size() > 0)
      logInfo("vulkan: pack shader files are OpenGL-only; using the built-in SPIR-V (assets/shaders/vk)");
    return 0;
  }

  // Cortana 2026-10-16 [ENGINE] (JD): entity meshes on Vulkan, at parity with GL33. The game
  // rebuilds the batch every frame (6 PackedVertex per quad, triangle list, positions relative to
  // setEntityOrigin). The verts are kept here and streamed into the recording frame slot's own
  // grow-only host-visible buffer (no race with a frame in flight), then drawn non-indexed with
  // the chunk pipeline and the origin as the push-constant offset. GL33 caps the batch at 4096
  // vertices; Vulkan draws it whole.
  void setEntityOrigin(float x, float y, float z) override { entityOrigin_[0] = x; entityOrigin_[1] = y; entityOrigin_[2] = z; }
  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override {
    entityVerts_.assign(verts.begin(), verts.begin() + std::ptrdiff_t(verts.size() / 3 * 3));
  }
  void uploadChunkMesh(const ChunkMesh& m) override {
    const int64_t key = meshKey(m.cx, m.sy, m.cz);
    const auto it = meshes_.find(key);
    if (it != meshes_.end()) {
      retire(it->second.slice);
      cullRemove(it->second);  // Phase 4.2a compact cull list
      meshes_.erase(it);
#if LL_STREAM_COUNTERS
      if (m.verts.empty()) ++perf_.evictions; else ++perf_.remeshes;  // Phase 4.0
#endif
    }
    if (m.verts.empty()) return;
    const auto tA = std::chrono::steady_clock::now();  // [C53-1] upload split
    PulledMesh& pm = pulledScratch();
    const bool hasBlend = meshHasBlend(m);
    VkDeviceSize bytes = 0;
    const auto slice = allocateMesh(m, hasBlend, pm, bytes);  // Mega-Pipeline Phase 2: packed quads when LL_VK_PULL=1
    const auto tB = std::chrono::steady_clock::now();
    if (!slice) {
      logError("vulkan: out of vertex memory; sub-chunk (%d,%d,%d) not drawn", m.cx, m.sy, m.cz);
      return;
    }
    const bool pulled = pm.active;
    if (uint8_t* dst = arena_->data(*slice)) writeMesh(m, pulled ? &pm : nullptr, dst);  // host / ReBAR: in place
    else if (!packed12_) stage(arena_->buffer(*slice), slice->offset, m.verts.data(), bytes);
    else {  // 0.0.1.59 [P59-1]: device-only block: pack into a scratch buffer, stage() copies it at once
      packScratch_.resize(size_t(bytes));
      writeMesh(m, pulled ? &pm : nullptr, packScratch_.data());
      stage(arena_->buffer(*slice), slice->offset, packScratch_.data(), bytes);
    }
    const auto tC = std::chrono::steady_clock::now();
    perf_.allocMs += std::chrono::duration<double, std::milli>(tB - tA).count();
    perf_.copyMs += std::chrono::duration<double, std::milli>(tC - tB).count();
    perf_.uploads += 1; perf_.uploadBytes += bytes;
    warnQuadTruncation(m);  // 0.0.1.56 F56-9 (review R8)
    Mesh gm{*slice, 0, m.cx, m.sy, m.cz};
    pulledLayout(m, pm, *slice, gm.indexCount, gm.pullFirst, gm.pullBucket);  // Mega-Pipeline Phase 2
    // JD Vulkan push: meshes with alpha-blend faces (bulb glass rims) join the sorted blend pass.
    gm.blend = hasBlend;
    for (int i = 0; i < 4; ++i) { gm.pulseP0[i] = m.pulseP0[i]; gm.pulseP1[i] = m.pulseP1[i]; gm.traceP0[i] = m.traceP0[i]; gm.traceP1[i] = m.traceP1[i]; }
    cullAdd(meshes_[key] = gm);  // Phase 4.2a
  }

  // 0.0.1.53 [C53-2] (Proposed by JD, Built by Cortana): worker-thread direct upload. With a ReBAR (or host)
  // arena the mesh worker allocates its slice and writes the packed vertices straight into
  // mapped memory, and does the blend scan; the main thread only publishes the handle.
  // LL_VK_DIRECT_UPLOAD=0 opts out (everything goes through uploadChunkMesh again).
  // 0.0.1.56 F56-9 (review R8; Discovered by Cortana review, Fixed by Cortana): meshes over kMaxQuads quads are
  // drawn truncated (shared index buffer limit). Say so once instead of silently dropping faces.
  // 0.0.1.59 [P59-1] Packed12: writes the mesh's GPU vertices (verts.size() * vertexStride_ bytes) to dst.
  // Out-of-domain vertices (z > 511 or layer > 32767) are clamped, counted and logged once; never wrapped.
  void writeVerts(const ChunkMesh& m, uint8_t* dst) {
    if (!packed12_) { std::memcpy(dst, m.verts.data(), m.verts.size() * sizeof(PackedVertex)); return; }
    if (const uint32_t misses = packVertices12(m.verts.data(), m.verts.size(), dst)) {
      if (packMisses_.fetch_add(misses, std::memory_order_relaxed) == 0)
        logWarn("vulkan: packed12: sub-chunk (%d,%d,%d) has %u vertices outside z<=511 / layer<=32767 (clamped). Logged once; total in the perf line.",
                m.cx, m.sy, m.cz, unsigned(misses));
    }
  }
  static void warnQuadTruncation(const ChunkMesh& m) {
    static std::atomic<bool> warned{false};
    if (m.verts.size() / 4 > kMaxQuads && !warned.exchange(true))
      logWarn("vulkan: sub-chunk (%d,%d,%d) has %zu quads; only the first %u are drawn (kMaxQuads). Logged once.",
              m.cx, m.sy, m.cz, m.verts.size() / 4, unsigned(kMaxQuads));
  }
  bool prepareChunkMeshAsync(const ChunkMesh& m, PreparedChunkMesh& out) override {
    out = PreparedChunkMesh{};
    if (!directUpload_ || m.verts.empty() || !arena_) return false;
    out.epoch = meshEpoch_.load(std::memory_order_acquire);  // F56-2: read before allocating
    PulledMesh& pm = pulledScratch();  // Mega-Pipeline Phase 2 (thread_local)
    out.blend = meshHasBlend(m);
    VkDeviceSize bytes = 0;
    const auto slice = allocateMesh(m, out.blend, pm, bytes);
    if (!slice) return false;
    uint8_t* dst = arena_->data(*slice);
    if (!dst) { arena_->free(*slice); return false; }  // device-only block: stage on the main thread
    writeMesh(m, &pm, dst);  // 0.0.1.59 [P59-1] (worker thread: pure packing; the miss counter is atomic)
    out.cx = m.cx; out.sy = m.sy; out.cz = m.cz;
    out.block = slice->block; out.offset = slice->offset; out.size = slice->size;
    pulledLayout(m, pm, *slice, out.indexCount, out.pullFirst, out.pullBucket);
    warnQuadTruncation(m);  // 0.0.1.56 F56-9 (review R8)
    for (int i = 0; i < 4; ++i) { out.pulseP0[i] = m.pulseP0[i]; out.pulseP1[i] = m.pulseP1[i]; out.traceP0[i] = m.traceP0[i]; out.traceP1[i] = m.traceP1[i]; }
    out.ready = true;
    directPrepared_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  void publishChunkMesh(const PreparedChunkMesh& p) override {
    if (!p.ready) return;
    if (p.epoch != meshEpoch_.load(std::memory_order_acquire)) {  // F56-2: prepared before a clear
      arena_->free(cVertexArena::Slice{p.block, p.offset, p.size});  // never drawn: free now
      if (staleDropped_++ == 0) logInfo("vulkan: dropped a worker-prepared chunk mesh from before clearChunkMeshes (slot freed)");
      return;
    }
    const int64_t key = meshKey(p.cx, p.sy, p.cz);
    const auto it = meshes_.find(key);
    if (it != meshes_.end()) {
      retire(it->second.slice);
      cullRemove(it->second);  // Phase 4.2a compact cull list
      meshes_.erase(it);
    }
    Mesh gm{cVertexArena::Slice{p.block, p.offset, p.size}, p.indexCount, p.cx, p.sy, p.cz};
    gm.blend = p.blend;
    gm.pullFirst = p.pullFirst;  // Mega-Pipeline Phase 2
    for (int i = 0; i < 7; ++i) gm.pullBucket[i] = p.pullBucket[i];
    for (int i = 0; i < 4; ++i) { gm.pulseP0[i] = p.pulseP0[i]; gm.pulseP1[i] = p.pulseP1[i]; gm.traceP0[i] = p.traceP0[i]; gm.traceP1[i] = p.traceP1[i]; }
    cullAdd(meshes_[key] = gm);  // Phase 4.2a
    perf_.published += 1;
  }
  void discardChunkMesh(const PreparedChunkMesh& p) override {
    if (p.ready) arena_->free(cVertexArena::Slice{p.block, p.offset, p.size});  // never drawn: free now
  }

  // PBR (BinaryQuad) path: Vulkan has no chunk_pbr pipeline. Loud once, not
  // silent (P1-RENDER H4, mirroring the uploadEntityMesh pattern above):
  // uploads and material tables are dropped, never quietly.
  void uploadPbrChunkMesh(const PbrChunkMesh&) override {
    static bool warned = false;
    if (!warned) { warned = true; logWarn("vulkan: uploadPbrChunkMesh not implemented — PBR (BinaryQuad) meshes invisible on Vulkan"); }
  }
  void setPbrMaterials(const cMaterialReg&) override {
    static bool warned = false;
    if (!warned) { warned = true; logWarn("vulkan: setPbrMaterials not implemented — no PBR material table on Vulkan"); }
  }

  void clearChunkMeshes() override {
    vkDeviceWaitIdle(dev_->device());
    for (FrameSlot& f : frames_) { f.copies.clear(); f.stagingUsed = 0; f.stagingSubmitted = false; f.overflow.clear(); f.overflowUsed = 0; }
    parked_.clear();  // [A52-4]
    for (const auto& [k, mesh] : meshes_) arena_->free(mesh.slice);
    for (const Retired& r : retired_) arena_->free(r.slice);
    meshes_.clear();
    cullList_.clear();  // Phase 4.2a
    retired_.clear();
    meshEpoch_.fetch_add(1, std::memory_order_acq_rel);  // F56-2: in-flight worker meshes are now stale
  }

  void renderFrame(const FrameParams& p) override {
    if (deviceLost_) return;  // F56-1: nothing more to draw on a lost device; the game closes
    if (p.width <= 0 || p.height <= 0) return;
    const bool tpOn = ::litholattice::tickprof::enabled();  // LL_TICK_PROFILE (measure only; Proposed by JD; co-implemented by JD and Cortana)
    int64_t tpT = tpOn ? ::litholattice::tickprof::nowNs() : 0;
    auto tpLap = [&](int ph) { if (tpOn) { const int64_t n = ::litholattice::tickprof::nowNs(); ::litholattice::tickprof::render().ns[ph] = n - tpT; tpT = n; } };
    // Sprite atlas (JD wiring 2026-10-13): (re)upload the pages when the
    // game's registry version moved — the GL33 contract, mirrored. The
    // upload waits for the device to go idle (same discipline as the
    // block texture array), so it happens here, before this frame's slot
    // is waited on and recorded.
    if (p.spriteQuads && p.spriteQuadCount > 0 && !p.spriteAtlasPages.empty() &&
        p.spriteAtlasVersion != spriteAtlasUploaded_)
      uploadSpriteAtlas(p);
    tpLap(::litholattice::tickprof::kSprite);  // LL_TICK_PROFILE
    // Culling-cache prototypes (JD 2026-10-11): arm per-frame state. All default off.
    cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
    FrameSlot& f = frames_[frameNumber_ % kFramesInFlight];
    const VkDevice d = dev_->device();
    const VkFence fence = f.inFlight.get();
    waitSlot(f);
    tpLap(::litholattice::tickprof::kWaitFence);  // LL_TICK_PROFILE
    if (deviceLost_) return;  // F56-1
    readPerf(f);  // 0.0.1.52 [A52-3]
    drainParked(f);  // 0.0.1.52 [A52-4]: uploads parked while this slot was busy
    releaseRetired();
    if (!swapchain_ || swapchainStale_ || requested_.width != uint32_t(p.width) || requested_.height != uint32_t(p.height))
      if (!recreateSwapchain(uint32_t(p.width), uint32_t(p.height))) return;
    tpLap(::litholattice::tickprof::kHousekeep);  // LL_TICK_PROFILE

    uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(d, swapchain_->handle(), UINT64_MAX, f.imageAcquired.get(), VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainStale_ = true; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { check(r, "vkAcquireNextImageKHR"); noteFatal(r, "vkAcquireNextImageKHR"); return; }
    tpLap(::litholattice::tickprof::kAcquire);  // LL_TICK_PROFILE
    vkResetFences(d, 1, &fence);
    record(f, image, p);
    tpLap(::litholattice::tickprof::kRecord);  // LL_TICK_PROFILE

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSemaphore acquired = f.imageAcquired.get(), done = renderDone_[image].get();
    auto si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    if (const VkResult sr = vkQueueSubmit(dev_->queue(), 1, &si, fence); sr != VK_SUCCESS) {
      // F56-1 (Cortana 2026-10-17, Discovered by Cortana review, Fixed by Cortana): the fence was reset above and the
      // acquire semaphore is signalled. Left alone, the next waitSlot() on this slot (UINT64_MAX) never
      // returns. An empty batch that waits on the semaphore and signals the fence keeps both valid; the
      // acquired image is never presented, so the swapchain is rebuilt next frame. If even that fails (or
      // the device is lost) the renderer stops and reports deviceLost() so the game closes cleanly.
      check(sr, "vkQueueSubmit");
      noteFatal(sr, "vkQueueSubmit");
      if (!deviceLost_) {
        auto ei = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        ei.waitSemaphoreCount = 1;
        ei.pWaitSemaphores = &acquired;
        ei.pWaitDstStageMask = &waitStage;
        const VkResult er = vkQueueSubmit(dev_->queue(), 1, &ei, fence);
        if (er != VK_SUCCESS) {
          check(er, "vkQueueSubmit (failed-submit recovery)");
          noteFatal(er, "vkQueueSubmit (recovery)");
          if (!deviceLost_) { deviceLost_ = true; logError("vulkan: cannot recover from a failed frame submit; the Vulkan renderer has stopped and LithoKraft will close"); }
        }
      }
      f.stagingSubmitted = true;  // the copies recorded into the failed batch are lost; recycle the ring
      swapchainStale_ = true;
      ++frameNumber_;
      return;
    }
    f.stagingSubmitted = true;  // its staging bytes are free again once this fence signals
    tpLap(::litholattice::tickprof::kSubmit);  // LL_TICK_PROFILE
    const VkSwapchainKHR sc = swapchain_->handle();
    auto pi = make<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &image;
    r = vkQueuePresentKHR(dev_->queue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainStale_ = true;  // rebuilt next frame
    else if (r != VK_SUCCESS) { check(r, "vkQueuePresentKHR"); noteFatal(r, "vkQueuePresentKHR"); }
    tpLap(::litholattice::tickprof::kPresent);  // LL_TICK_PROFILE
    if (tpOn) ::litholattice::tickprof::render().valid = true;
    ++frameNumber_;
  }

  RenderStats stats() const override { return stats_; }
  bool deviceLost() const override { return deviceLost_; }  // F56-1
  float gpuFrameMs() const override { return lastGpuMs_; }   // F56-4
  void requestGrab() override { grabArmed_ = true; }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (!grabReady_ || !grabStaging_) return false;
    FrameSlot& f = frames_[grabSlot_ % kFramesInFlight];
    // Wait for the armed frame (which carried the copy) to finish on the GPU.
    const VkFence fence = f.inFlight.get();
    if (vkWaitForFences(dev_->device(), 1, &fence, VK_TRUE, 1000000000ULL) != VK_SUCCESS)
      return false;
    w = int(grabExtent_.width);
    h = int(grabExtent_.height);
    const bool bgra = grabFormat_ == VK_FORMAT_B8G8R8A8_UNORM;
    rgb.resize(size_t(w) * size_t(h) * 3);
    const uint8_t* src = grabStaging_->mapped();
    // Vulkan images are top-left origin: rows are already top-first for PNG.
    for (int y = 0; y < h; ++y) {
      const uint8_t* srow = src + size_t(y) * size_t(w) * 4;
      uint8_t* drow = rgb.data() + size_t(y) * size_t(w) * 3;
      for (int x = 0; x < w; ++x) {
        drow[0] = srow[bgra ? 2 : 0];
        drow[1] = srow[1];
        drow[2] = srow[bgra ? 0 : 2];
        srow += 4;
        drow += 3;
      }
    }
    return true;
  }
  void setFrustumMarginDeg(float degrees) override {
    frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11
  }
  void setCullProtoConfig(const CullProtoConfig& cfg) override { cullProto_.configure(cfg); }
  CullProtoStats cullProtoStats() const override { return cullProto_.stats(); }

 private:
  struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // freed with the pool
    VkQueryPool timer = VK_NULL_HANDLE;    ///< 0.0.1.52 [A52-3]: 2 timestamps (frame start / end)
    bool timerWritten = false;
    UniqueFence inFlight;
    UniqueSemaphore imageAcquired;
    std::optional<Buffer> uniforms, overlay;
    // Sprite pass vertex stream (JD wiring 2026-10-13): per-slot so a frame
    // in flight never races the next frame's upload; grow-only like GL33.
    std::optional<Buffer> sprites;
    VkDeviceSize spriteCapacityVerts = 0;
    std::optional<Buffer> entities;  ///< Cortana 2026-10-16: entity batch stream (grow-only, per slot)
    VkDeviceSize entityCapacityVerts = 0;
    // Staging ring for device-local vertex uploads made before this slot's next frame; the
    // copies are recorded at the start of its command buffer. Reusable once its fence signals.
    std::optional<Buffer> staging;
    VkDeviceSize stagingUsed = 0;
    bool stagingSubmitted = false;
    struct Copy { VkBuffer src, dst; VkBufferCopy region; };  // [A52-4]: source may be an overflow buffer
    std::vector<Copy> copies;
    std::vector<Buffer> overflow;  ///< [A52-4] extra staging for this frame; freed once the fence signals
    VkDeviceSize overflowUsed = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;  // freed with the descriptor pool
    // Mega-draw V2/V3 (Direction JD, design Archie, implementation Cortana): per-draw records (binding 1,
    // instance rate) and indirect commands for this slot's chunk draws; grow-only.
    std::optional<Buffer> megaDraws, megaCmds;
    VkDeviceSize megaCapacity = 0;
    VkBuffer arenaSet[kMaxPullBlocks]{};  ///< Mega-Pipeline Phase 2: buffers last written to binding 6
  };
  struct Mesh {
    cVertexArena::Slice slice;
    uint32_t indexCount = 0;
    int cx = 0, sy = 0, cz = 0;
    float pulseP0[4]{};  ///< wafer ground-pulse blended params (from ChunkMesh)
    float pulseP1[4]{};
    float traceP0[4]{};  ///< wafer ground-trace blended params (from ChunkMesh)
    float traceP1[4]{};
    bool blend = false;  ///< JD Vulkan push: has kShadeAlphaBlend faces
    uint32_t pullFirst = 0;   ///< Mega-Pipeline Phase 2: first packed quad (8-byte index in the arena block)
    uint32_t pullBucket[7]{};  ///< Mega-Pipeline Phase 2/3: packed quads by face; pullBucket[6] = packed quad count
    uint32_t cullIdx = UINT32_MAX;  ///< Phase 4.2a: slot in cullList_ (last member: keeps the aggregate inits)
  };
  struct Retired {
    uint64_t retiredAt = 0;  ///< frameNumber_ when replaced: frames before it may still read it
    cVertexArena::Slice slice;
  };

  static bool fail(std::string* error, const char* what) {
    *error = std::string("Vulkan init failed: ") + what;
    return false;
  }

  bool createStaticBuffers() {
    const auto hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // 0.0.1.52 [A52-2] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): 16-bit quad indices. The largest index is
    // kMaxQuads * 4 - 1 = 65535, which fits; primitive restart is off in every pipeline
    // (vk_pipelines.cpp), so 0xFFFF is an ordinary vertex index, not a strip cut.
    static_assert(uint64_t(kMaxQuads) * 4 - 1 <= 0xFFFFu, "quad indices must fit uint16");
    const VkDeviceSize idxBytes = VkDeviceSize(kMaxQuads) * 6 * sizeof(uint16_t);
    auto host = Buffer::create(*dev_, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible);
    if (!host) return false;
    auto* idx = reinterpret_cast<uint16_t*>(host->mapped());
    for (uint32_t q = 0; q < kMaxQuads; ++q) {
      const uint32_t b = q * 4;
      const uint16_t quad[6] = {uint16_t(b), uint16_t(b + 1), uint16_t(b + 2), uint16_t(b), uint16_t(b + 2), uint16_t(b + 3)};
      std::memcpy(idx + q * 6, quad, sizeof quad);
    }
    if (arena_->kind() == cVertexArena::Kind::Host) {
      quadIndices_ = std::move(host);
    } else {  // device-local: one copy at init
      quadIndices_ = Buffer::create(*dev_, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!quadIndices_) return false;
      const VkBuffer src = host->handle(), dst = quadIndices_->handle();
      if (!submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) {
            const VkBufferCopy region{0, 0, idxBytes};
            vkCmdCopyBuffer(c, src, dst, 1, &region);
          }))
        return false;
    }
    const HudFontAtlas atlas = buildHudFontAtlas();
    font_ = uploadImage2D(*dev_, pool_.get(), VK_FORMAT_R8_UNORM, uint32_t(atlas.width), uint32_t(atlas.height),
                          atlas.pixels.data(), 1);
    fontSampler_ = createNearestSampler(*dev_);
    spriteSampler_ = createLinearSampler(*dev_);
    // Sprite atlas starts as a 1x1x1 dummy array so descriptor binding 4 is
    // always valid; real pages replace it on the first sprite frame whose
    // atlas version differs (uploadSpriteAtlas).
    if (!uploadSpritePages(std::span<const ::litholattice::Image>{}, 0)) return false;
    return font_.has_value() && fontSampler_ && spriteSampler_;
  }

  bool createFrames() {
    const VkDevice d = dev_->device();
    const auto hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (FrameSlot& f : frames_) {
      auto ai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
      ai.commandPool = pool_.get();
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      auto fi = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // the first wait returns at once
      VkFence fence = VK_NULL_HANDLE;
      VkSemaphore sem = VK_NULL_HANDLE;
      if (!check(vkAllocateCommandBuffers(d, &ai, &f.cmd), "vkAllocateCommandBuffers") ||
          !check(vkCreateFence(d, &fi, nullptr, &fence), "vkCreateFence"))
        return false;
      f.inFlight = UniqueFence(d, fence);
      {  // 0.0.1.52 [A52-3]: GPU frame timer (optional: a failed pool just disables it)
        auto qi = make<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;  // V2b: + chunk pass begin/end
        if (vkCreateQueryPool(d, &qi, nullptr, &f.timer) != VK_SUCCESS) f.timer = VK_NULL_HANDLE;
      }
      if (!createSemaphore(sem)) return false;
      f.imageAcquired = UniqueSemaphore(d, sem);
      f.uniforms = Buffer::create(*dev_, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVisible);
      f.overlay = Buffer::create(*dev_, kOverlayBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible);
      if (!f.uniforms || !f.overlay) return false;
      if (arena_->kind() != cVertexArena::Kind::Host) {
        f.staging = Buffer::create(*dev_, kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible);
        if (!f.staging) return false;
      }
    }
    return true;
  }

  bool createSemaphore(VkSemaphore& out) const {
    auto si = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    return check(vkCreateSemaphore(dev_->device(), &si, nullptr, &out), "vkCreateSemaphore");
  }

  bool createDescriptors() {
    const VkDevice d = dev_->device();
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight * (1 + kMaxPullBlocks)}};  // mega-draw V2b + Phase 2 arena array
    auto ci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    ci.maxSets = kFramesInFlight;
    ci.poolSizeCount = uint32_t(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorPool(d, &ci, nullptr, &pool), "vkCreateDescriptorPool")) return false;
    descriptorPool_ = UniqueDescriptorPool(d, pool);
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(pipelines_->setLayout());
    auto ai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ai.descriptorPool = pool;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> sets{};
    if (!check(vkAllocateDescriptorSets(d, &ai, sets.data()), "vkAllocateDescriptorSets")) return false;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) frames_[i].set = sets[i];
    // JD Vulkan push: bloom sets (glow, blur A, blur B), rewritten with each swapchain.
    const VkDescriptorPoolSize postSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    auto pci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pci.maxSets = 3;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = postSizes;
    VkDescriptorPool ppool = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorPool(d, &pci, nullptr, &ppool), "vkCreateDescriptorPool (post)")) return false;
    postPool_ = UniqueDescriptorPool(d, ppool);
    const VkDescriptorSetLayout pl[3] = {pipelines_->postSetLayout(), pipelines_->postSetLayout(), pipelines_->postSetLayout()};
    auto pai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    pai.descriptorPool = ppool;
    pai.descriptorSetCount = 3;
    pai.pSetLayouts = pl;
    if (!check(vkAllocateDescriptorSets(d, &pai, postSets_), "vkAllocateDescriptorSets (post)")) return false;
    return true;
  }

  // Points every frame's set at the current images (device idle: after init or a texture upload).
  void writeDescriptors() {
    const VkDescriptorImageInfo array = textures_->arrayDescriptor(), palette = textures_->paletteDescriptor();
    const VkDescriptorImageInfo font{fontSampler_.get(), font_->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo spriteAtlas{spriteSampler_.get(),
                                            spriteAtlas_ ? spriteAtlas_->view() : VK_NULL_HANDLE,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (const FrameSlot& f : frames_) {
      const VkDescriptorBufferInfo ubo{f.uniforms->handle(), 0, sizeof(FrameUniforms)};
      VkWriteDescriptorSet w[5]{};
      for (uint32_t i = 0; i < 5; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = f.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      }
      w[kBindingFrame].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[kBindingFrame].pBufferInfo = &ubo;
      w[kBindingTextureArray].pImageInfo = &array;
      w[kBindingPalette].pImageInfo = &palette;
      w[kBindingFont].pImageInfo = &font;
      w[kBindingSpriteAtlas].pImageInfo = &spriteAtlas;
      vkUpdateDescriptorSets(dev_->device(), 5, w, 0, nullptr);
    }
  }

  bool recreateSwapchain(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(dev_->device());
    auto next = cSwapchain::create(*dev_, surface_, pipelines_->renderPass(), pipelines_->compositePass(),
                                  pipelines_->blurPass(), RenderPipelines::kGlowFormat, width, height, swapchain_.get());
    swapchain_ = std::move(next);
    if (!swapchain_) {  // minimized or lost: try again next frame
      // 0.0.1.56 F56-8 (review R6; Discovered by Cortana review, Fixed by Cortana): a zero surface extent with a
      // non-zero window made every frame run vkDeviceWaitIdle + a caps query with no pacing (CPU spin while minimized).
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      return false;
    }
    {  // JD Vulkan push: point the bloom sets at this swapchain's glow / blur images
      const VkImageView views[3] = {swapchain_->glowView(), swapchain_->blurView(0), swapchain_->blurView(1)};
      VkDescriptorImageInfo infos[3];
      VkWriteDescriptorSet w[3]{};
      for (int i = 0; i < 3; ++i) {
        infos[i] = {spriteSampler_.get(), views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};  // linear, clamp
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = postSets_[i];
        w[i].dstBinding = 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &infos[i];
      }
      vkUpdateDescriptorSets(dev_->device(), 3, w, 0, nullptr);
    }
    requested_ = {width, height};
    swapchainStale_ = false;
    renderDone_.clear();
    for (uint32_t i = 0; i < swapchain_->imageCount(); ++i) {
      VkSemaphore s = VK_NULL_HANDLE;
      if (!createSemaphore(s)) return false;
      renderDone_.emplace_back(dev_->device(), s);
    }
    return true;
  }

  void retire(const cVertexArena::Slice& s) { retired_.push_back({frameNumber_, s}); }
  // F56-1 (Cortana 2026-10-17): VK_ERROR_DEVICE_LOST (driver reset / TDR / GPU fault) is not recoverable here:
  // stop rendering, say so once, and let the game close through its normal shutdown path.
  void noteFatal(VkResult r, const char* what) {
    if (r != VK_ERROR_DEVICE_LOST || deviceLost_) return;
    deviceLost_ = true;
    logError("vulkan: GPU device lost during %s (driver reset, TDR or GPU fault). The Vulkan renderer has stopped; "
             "LithoKraft will close normally. Update the GPU driver or try --renderer gl33 if this repeats.", what);
  }
  bool deviceLost_ = false;
  // F56-2 (Cortana 2026-10-17): chunk-mesh epoch. clearChunkMeshes() bumps it; a worker-prepared mesh stamped with
  // an older epoch is freed instead of published (it was allocated before / during the clear).
  std::atomic<uint64_t> meshEpoch_{1};
  uint64_t staleDropped_ = 0;
  float lastGpuMs_ = -1.f;  ///< F56-4: last measured GPU frame time (ms), -1 = no timer
  const bool directUpload_ = [] {  // [C53-2]
    const char* e = std::getenv("LL_VK_DIRECT_UPLOAD");
    return !(e && e[0] == '0');
  }();
  std::atomic<uint64_t> directPrepared_{0};
  const float heartbeatEnv_ = [] {  // [C53-3]
    const char* e = std::getenv("LL_VK_HEARTBEAT_GLOW");
    return e ? float(std::atof(e)) : -1.f;
  }();
  // 0.0.1.59 [P59-1] Packed12 (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the
  // Bitmaster and the Bitmasker; layout correction and build by Cortana): 12-byte chunk vertices.
  // LL_VK_PACKED12=0 keeps the 16-byte path (fallback), =1 forces Packed12; unset = kPacked12Default.
  static constexpr bool kPacked12Default = true;
  // 0.0.1.59-b F59-1 (BUG-0031; Discovered by Cortana (review), Fixed by Cortana): parse the value; "off" /
  // "false" / "no" used to switch Packed12 ON (only a leading '0' meant off). Unknown values keep the default.
  const bool packed12_ = [] {
    const char* e = std::getenv("LL_VK_PACKED12");
    if (!e || !e[0]) return kPacked12Default;
    std::string v(e);
    for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (v == "0" || v == "off" || v == "false" || v == "no") return false;
    if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
    logWarn("vulkan: LL_VK_PACKED12='%s' not understood (use 1/0); keeping the default (%s)", e, kPacked12Default ? "on" : "off");
    return kPacked12Default;
  }();
  // Mega-draw V1 (Direction JD, design Archie, implementation Cortana): LL_VK_MEGA selects the chunk draw path.
  // 0 = per-draw push constants (old path), 1 = per-draw instance buffer (V2), 2 = indirect draws per arena block (V3),
  // 3 = V3 + storage-buffer records (V2b; default since 2026-10-25 by JD's call). Fallbacks: missing features,
  // pipelines or buffers drop to V2 direct draws or the old path.
  const int megaMode_ = [] {
    const char* e = std::getenv("LL_VK_MEGA");
    if (!e || !*e) return 3;
    const int v = std::atoi(e);
    if (v < 0 || v > 3) { logWarn("vulkan: LL_VK_MEGA='%s' not understood (use 0/1/2/3); keeping the default 3", e); return 3; }
    return v;
  }();
  // Mega-draw V2b (Direction JD, design Archie, implementation Cortana): LL_VK_MEGA=3 = V3 indirect draws + records
  // read from a storage buffer by draw index. LL_VK_MEGA_PROBE=1 (profiling only) swaps in the variant that reads
  // the storage buffer in the vertex stage and passes flat vec4 varyings. LL_VK_FREEZE_TIME=<s> pins the shader
  // clocks (screenshot diffs); render-only, unset = live clocks.
  const bool megaProbe_ = [] { const char* e = std::getenv("LL_VK_MEGA_PROBE"); return e && std::atoi(e) == 1; }();
  const float freezeTime_ = [] { const char* e = std::getenv("LL_VK_FREEZE_TIME"); return e && *e ? float(std::atof(e)) : -1.f; }();
  const VkDeviceSize vertexStride_ = packed12_ ? sizeof(GpuVertex12) : sizeof(PackedVertex);
  std::atomic<uint64_t> packMisses_{0};
  std::vector<uint8_t> packScratch_;  ///< main thread only (device-arena staging path)
  bool pull_ = false;     ///< Mega-Pipeline Phase 2 (LL_VK_PULL=1)
  bool blendSplit_ = true;  ///< Phase 4 B (Archie Q1): with pulling, blended meshes pull their opaque quads (LL_VK_BLENDSPLIT=0: off)
  std::atomic<uint64_t> pullWhy_[kPullReasons] = {};  ///< Phase 4.0: quads by pull outcome (cumulative; LL_VK_PULL=1 only)
  bool buckets_ = false;  ///< Mega-Pipeline Phase 3 (LL_VK_BUCKETS=1)
  std::vector<const Mesh*> megaMesh_;  ///< Phase 2: mesh per opaque record (pulled pass)
  static PulledMesh& pulledScratch() { thread_local PulledMesh pm; return pm; }
  static bool meshHasBlend(const ChunkMesh& m) {
    for (const PackedVertex& v : m.verts)
      if ((v.mode & 63) == kShadeAlphaBlend) return true;
    return false;
  }
  // Phase 2: allocates the slice. With pulling on (and no blend faces) the mesh is split into packed quads +
  // Packed12 fallback (pm.active); a slice landing on block >= kMaxPullBlocks is redone on the plain layout.
  std::optional<cVertexArena::Slice> allocateMesh(const ChunkMesh& m, bool hasBlend, PulledMesh& pm, VkDeviceSize& bytes) {
    pm.active = false;
#if LL_STREAM_COUNTERS
    if (pull_ && hasBlend && !blendSplit_) pullWhy_[kPullBlendMesh].fetch_add(uint32_t(m.verts.size() / 4), std::memory_order_relaxed);
#endif
    if (pull_ && (!hasBlend || blendSplit_)) {  // Phase 4 B: blended meshes split (LL_VK_BLENDSPLIT=0 opts out)
      uint32_t why[kPullReasons] = {};  // Phase 4.0 quad-rejection counter (for Archie)
      buildPulled(m.verts, kMaxQuads, true, pm, LL_STREAM_COUNTERS ? why : nullptr, hasBlend ? int(kShadeAlphaBlend) : -1);
#if LL_STREAM_COUNTERS
      for (uint32_t r = 1; r < kPullReasons; ++r)
        if (why[r]) pullWhy_[r].fetch_add(why[r], std::memory_order_relaxed);
      pullWhy_[kPullOk].fetch_add(uint32_t(pm.quads.size()), std::memory_order_relaxed);
#endif
      if (!pm.quads.empty()) {
        bytes = VkDeviceSize(pulledBytes(pm));
        auto s = arena_->allocate(bytes);
        if (s && s->block < kMaxPullBlocks) { pm.active = true; return s; }
#if LL_STREAM_COUNTERS
        if (s) {
          pullWhy_[kPullBlock].fetch_add(uint32_t(pm.quads.size()), std::memory_order_relaxed);
          pullWhy_[kPullOk].fetch_sub(uint32_t(pm.quads.size()), std::memory_order_relaxed);
        }
#endif
        if (s) arena_->free(*s);
      }
    }
    bytes = m.verts.size() * vertexStride_;  // 0.0.1.59 [P59-1]: 12 (Packed12) or 16
    return arena_->allocate(bytes);
  }
  void writeMesh(const ChunkMesh& m, const PulledMesh* pm, uint8_t* dst) {
    if (!pm || !pm->active) { writeVerts(m, dst); return; }
    if (const uint32_t misses = writePulled(*pm, dst)) packMisses_.fetch_add(misses, std::memory_order_relaxed);
  }
  // Phase 2: index count (fallback quads) and pulled-quad placement for a slice.
  void pulledLayout(const ChunkMesh& m, const PulledMesh& pm, const cVertexArena::Slice& s, uint32_t& indexCount,
                    uint32_t& pullFirst, uint32_t* bucket) const {
    if (!pm.active) {
      indexCount = std::min<uint32_t>(uint32_t(m.verts.size() / 4), kMaxQuads) * 6;
      pullFirst = 0;
      for (int i = 0; i < 7; ++i) bucket[i] = 0;
      return;
    }
    indexCount = uint32_t(pm.fallback.size() / 4) * 6;
    pullFirst = uint32_t((s.offset + pm.fallback.size() * sizeof(GpuVertex12)) / 8 + pm.palette.size());
    for (int i = 0; i < 7; ++i) bucket[i] = pm.bucket[i];
  }

  // Waits until the GPU is done with `f`; recycles its staging ring if its copies were submitted.
  void waitSlot(FrameSlot& f) {
    const VkFence fence = f.inFlight.get();
    if (deviceLost_) return;  // F56-1
    if (const VkResult wr = vkWaitForFences(dev_->device(), 1, &fence, VK_TRUE, UINT64_MAX); wr != VK_SUCCESS) {
      check(wr, "vkWaitForFences");
      noteFatal(wr, "vkWaitForFences");
      return;
    }
    if (f.stagingSubmitted) { f.stagingUsed = 0; f.stagingSubmitted = false; f.overflow.clear(); f.overflowUsed = 0; }
  }

  // Device-local upload: bytes go into the next frame's staging ring, the copy is recorded at the
  // start of that frame (then a transfer -> vertex-input barrier). A ring that is full is flushed
  // with a one-off submit; a mesh larger than the ring goes through a temporary buffer.
  // 0.0.1.52 [A52-4] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): non-blocking staging. Uploads made while the
  // slot's previous frame is still on the GPU are parked in CPU memory and copied into the ring
  // right after renderFrame's own fence wait (no early wait in the streaming phase). A full ring
  // spills into a per-slot overflow buffer instead of a vkQueueWaitIdle flush.
  void stage(VkBuffer dst, VkDeviceSize dstOffset, const void* src, VkDeviceSize bytes) {
    FrameSlot& f = frames_[frameNumber_ % kFramesInFlight];
    if (vkGetFenceStatus(dev_->device(), f.inFlight.get()) != VK_SUCCESS) {
      Parked pk{dst, dstOffset, {}};
      pk.data.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + bytes);
      parked_.push_back(std::move(pk));
      return;
    }
    waitSlot(f);  // signalled: returns at once (recycles the ring if its copies were submitted)
    stageNow(f, dst, dstOffset, src, bytes);
  }
  void stageNow(FrameSlot& f, VkBuffer dst, VkDeviceSize dstOffset, const void* src, VkDeviceSize bytes) {
    if (f.stagingUsed + bytes <= kStagingBytes) {
      std::memcpy(f.staging->mapped() + f.stagingUsed, src, size_t(bytes));
      f.copies.push_back({f.staging->handle(), dst, VkBufferCopy{f.stagingUsed, dstOffset, bytes}});
      f.stagingUsed += (bytes + 15) & ~VkDeviceSize(15);
      return;
    }
    if (f.overflow.size() >= 4 && f.overflowUsed + bytes > f.overflow.back().size()) {
      // F56-5 (Cortana 2026-10-17, review): no frame has consumed this slot's copies (window minimized: renderFrame
      // is not called, but streaming keeps uploading), so overflow buffers grew without bound. Callers
      // only stage into a slot whose fence has signalled, so flush the pending copies now (one blocking
      // submit) and start the ring over. Bounded at ring + 4 overflow buffers (~160 MB).
      submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) { recordCopies(c, f); });
      f.copies.clear(); f.stagingUsed = 0; f.overflow.clear(); f.overflowUsed = 0;
      stageNow(f, dst, dstOffset, src, bytes);
      return;
    }
    if (f.overflow.empty() || f.overflowUsed + bytes > f.overflow.back().size()) {
      auto ob = Buffer::create(*dev_, std::max(bytes, kStagingBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!ob) { logError("vulkan: staging overflow allocation failed; upload dropped"); return; }
      f.overflow.push_back(std::move(*ob));
      f.overflowUsed = 0;
    }
    Buffer& ob = f.overflow.back();
    std::memcpy(ob.mapped() + f.overflowUsed, src, size_t(bytes));
    f.copies.push_back({ob.handle(), dst, VkBufferCopy{f.overflowUsed, dstOffset, bytes}});
    f.overflowUsed += (bytes + 15) & ~VkDeviceSize(15);
  }
  void drainParked(FrameSlot& f) {
    for (const Parked& pk : parked_) stageNow(f, pk.dst, pk.offset, pk.data.data(), VkDeviceSize(pk.data.size()));
    parked_.clear();
  }

  void recordCopies(VkCommandBuffer cmd, FrameSlot& f) {
    if (f.copies.empty()) return;
    for (const auto& c : f.copies) vkCmdCopyBuffer(cmd, c.src, c.dst, 1, &c.region);  // [A52-4]
    auto mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    f.copies.clear();
  }

  // Staging ring full (many uploads before one frame, e.g. a dimension load): copy now and wait.
  void flushCopiesNow(FrameSlot& f) {
    if (!f.copies.empty()) submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) { recordCopies(c, f); });
    f.copies.clear();
    f.stagingUsed = 0;
  }

  // Called after waiting for this slot's fence: every frame before frameNumber_ + 1 - kFramesInFlight is done.
  void releaseRetired() {
    std::erase_if(retired_, [&](const Retired& r) {
      if (frameNumber_ + 1 < kFramesInFlight + r.retiredAt) return false;
      arena_->free(r.slice);
      return true;
    });
  }

  void writeUniforms(const FrameSlot& f, const FrameParams& p) {
    FrameUniforms u = materials_;
    std::memcpy(u.viewProj, p.viewProj.m, sizeof u.viewProj);
    u.cam[0] = p.camPos.x; u.cam[1] = p.camPos.y; u.cam[2] = p.camPos.z;
    u.sky[0] = p.sky.x; u.sky[1] = p.sky.y; u.sky[2] = p.sky.z;
    u.fogEnd = p.fogEnd;
    u.time = p.timeSec; u.debug = p.debugOverlay ? 1 : 0;
    u.pulseTime = p.groundPulseTimeSec; u.pulseOn = p.groundPulseOn ? 1.f : 0.f;  // wafer ground pulse
    // 0.0.1.53 [C53-3] (Proposed by JD, Built by Cortana): heartbeat bloom gain in uPulse.z; LL_VK_HEARTBEAT_GLOW overrides.
    u.pulsePad[0] = std::clamp(heartbeatEnv_ >= 0.f ? heartbeatEnv_ : p.vkHeartbeatGlow, 0.f, 4.f);
    u.traceTime = p.groundTraceTimeSec; u.traceOn = p.groundTraceOn ? 1.f : 0.f;  // wafer ground traces
    u.tile = textures_->tileSize();
    u.twinkle = p.leafTwinkle;  // 2026-10-16 crystal twinkle
    // JD Vulkan push (Cortana 2026-10-16): effect toggles; pulse on the sim clock (render-only).
    u.fx[0] = p.vkFx;
    u.fx[1] = std::clamp(p.vkParticleCount, 0, 65536);
    u.fxP[0] = std::max(0.f, p.vkBloomStrength);
    u.fxP[1] = std::max(0.f, p.vkFogDensity);
    u.fxP[2] = p.groundPulseTimeSec;
    u.fxP[3] = 0.5f;
    if (freezeTime_ >= 0.f) u.time = u.pulseTime = u.traceTime = u.fxP[2] = freezeTime_;  // V2b: LL_VK_FREEZE_TIME
    std::memcpy(f.uniforms->mapped(), &u, sizeof u);
  }

  void record(FrameSlot& f, uint32_t image, const FrameParams& p) {
    writeUniforms(f, p);
    if (megaMode_ > 0) ensureMega(f, meshes_.size() * megaPerMesh());  // mega-draw V2b: grow (and rewrite binding 5) before the set is bound
    if (pull_ && f.megaDraws) updateArenaSet(f);  // Mega-Pipeline Phase 2: binding 6 = arena blocks
    const VkCommandBuffer cmd = f.cmd;
    auto bi = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (f.timer) {  // [A52-3]
      vkCmdResetQueryPool(cmd, f.timer, 0, 4);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.timer, 0);
    }
    recordCopies(cmd, f);  // this frame's vertex uploads land before any draw reads them
    VkClearValue clear[3]{};
    clear[0].color = {{p.sky.x, p.sky.y, p.sky.z, 1.f}};
    clear[1].depthStencil = {1.f, 0};
    clear[2].color = {{0.f, 0.f, 0.f, 0.f}};  // JD Vulkan push: glow target
    const VkExtent2D extent = swapchain_->extent();
    auto rp = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rp.renderPass = pipelines_->renderPass();
    rp.framebuffer = swapchain_->framebuffer(image);
    rp.renderArea = {{0, 0}, extent};
    rp.clearValueCount = 3;
    rp.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0.f, 1.f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layout(), 0, 1, &f.set, 0, nullptr);
    {
      const auto t0 = std::chrono::steady_clock::now();
      if (f.timer) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timer, 2);  // V2b profiling
      drawChunks(cmd, f, p);
      if (f.timer) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timer, 3);
      perf_.chunkCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    drawEntities(cmd, f);  // Cortana 2026-10-16: after the world, before sprites (GL33 order)
    drawSprites(cmd, f, p);  // sprite pass: after world geometry, before lines/HUD (GL33 order)
    drawParticles(cmd, p);   // JD Vulkan push
    vkCmdEndRenderPass(cmd);
    // JD Vulkan push (Cortana 2026-10-16): bloom (half-res separable blur of the glow target, two
    // iterations), then the composite pass adds it and draws lines + HUD on top, unbloomed.
    const bool bloom = (p.vkFx & FrameParams::kVkFxBloom) != 0 && p.vkBloomStrength > 0.f;
    if (bloom) drawBloomBlur(cmd);
    auto cp = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    cp.renderPass = pipelines_->compositePass();
    cp.framebuffer = swapchain_->compositeFramebuffer(image);
    cp.renderArea = {{0, 0}, extent};
    vkCmdBeginRenderPass(cmd, &cp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (bloom) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->composite());
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->postLayout(), 0, 1, &postSets_[2], 0, nullptr);
      const PostPush pp{{0.f, 0.f, std::min(p.vkBloomStrength, 4.f), 1.f}};
      vkCmdPushConstants(cmd, pipelines_->postLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
      vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layout(), 0, 1, &f.set, 0, nullptr);
    drawOverlay(cmd, f, p);
    vkCmdEndRenderPass(cmd);
    // Screenshot grab (lithoscript `screenshot`): the game arms the grab during
    // update, before the frame is drawn. Copy the just-rendered swapchain image
    // to a host staging buffer before present (image-to-host copy).
    if (grabArmed_) {
      grabArmed_ = false;
      recordGrab(cmd, uint32_t(frameNumber_ % kFramesInFlight), image);
    }
    if (f.timer) { vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.timer, 1); f.timerWritten = true; }  // [A52-3]
    vkEndCommandBuffer(cmd);
  }

  // 0.0.1.52 [A52-3]: after the slot's fence: read its GPU time; log the counters every 120 frames.
  void readPerf(FrameSlot& f) {
    if (f.timer && f.timerWritten) {
      uint64_t ts[4] = {0, 0, 0, 0};
      if (vkGetQueryPoolResults(dev_->device(), f.timer, 0, 4, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
        if (ts[3] > ts[2]) perf_.chunkGpuMs += double(ts[3] - ts[2]) * timestampPeriodNs_ * 1e-6;  // V2b
        const double ms = double(ts[1] - ts[0]) * timestampPeriodNs_ * 1e-6;
        perf_.gpuMs += ms; perf_.gpuMaxMs = std::max(perf_.gpuMaxMs, ms); ++perf_.gpuFrames;
        lastGpuMs_ = lastGpuMs_ < 0.f ? float(ms) : lastGpuMs_ * 0.9f + float(ms) * 0.1f;  // F56-4 (smoothed)
      }
      f.timerWritten = false;
    }
    if (++perf_.frames >= 120) {  // [A52-6] every 120 frames (benches run ~400 frames)
      const double n = double(perf_.frames);
      logInfo("vulkan perf: %llu frames: chunk draws/frame %.0f, vertex-buffer binds/frame %.1f (one per draw before 0.0.1.52), push bytes/frame %.0f (%.0f before), gpu ms avg %.2f max %.2f, arena device %llu MB host %llu MB, mesh %.1f MB (%s), packed12 clamps %llu",
              (unsigned long long)perf_.frames, double(perf_.draws) / n, double(perf_.binds) / n, double(perf_.pushBytes) / n,
              double(perf_.draws) * 80.0 / n, perf_.gpuFrames ? perf_.gpuMs / double(perf_.gpuFrames) : 0.0, perf_.gpuMaxMs,
              (unsigned long long)(arena_->deviceBytes() >> 20), (unsigned long long)(arena_->hostBytes() >> 20),
              double(arena_->usedBytes()) / 1048576.0, packed12_ ? "packed12" : "packed16",
              (unsigned long long)packMisses_.load(std::memory_order_relaxed));  // 0.0.1.59 [P59-1]
      logInfo("vulkan mega: mode %d%s, logical chunk draws/frame %.0f, draw API calls/frame %.0f, chunk record cpu ms/frame %.3f, chunk gpu ms %.3f",
              megaMode_, megaProbe_ ? " (probe)" : "", double(perf_.draws) / n, double(perf_.apiCalls) / n, perf_.chunkCpuMs / n,
              perf_.gpuFrames ? perf_.chunkGpuMs / double(perf_.gpuFrames) : 0.0);  // mega-draw V2
#if LL_STREAM_COUNTERS
      {  // Phase 4.0 (Direction JD, research Elfie, implementation Cortana)
        const double cap = double(arena_->blockCount()) * 32.0;
        logInfo("vulkan stream: uploads/frame %.2f (worker-published %.2f), upload MB/frame %.3f, evictions/frame %.2f, remeshes/frame %.2f, "
                "chunks tested/frame %.0f, frustum-culled %.0f, occlusion-culled 0 (no occlusion pass), arena %.1f MB used of %.0f MB reserved (%.1f%%)",
                double(perf_.uploads) / n, double(perf_.published) / n, double(perf_.uploadBytes) / n / 1048576.0, double(perf_.evictions) / n,
                double(perf_.remeshes) / n, double(perf_.cullTested) / n, double(perf_.cullFrustum) / n, double(arena_->usedBytes()) / 1048576.0, cap,
                cap > 0 ? 100.0 * double(arena_->usedBytes()) / 1048576.0 / cap : 0.0);
        logInfo("vulkan record split (ms/frame): cull %.3f, sort %.3f, opaque put+draw %.3f, blend sort+put+draw %.3f (blend draws/frame %.0f)",
                perf_.cullMs / n, perf_.sortMs / n, perf_.opaqueMs / n, perf_.blendMs / n, double(perf_.blendDraws) / n);
        if (pull_) {
          char buf[512]; int o = 0;
          for (uint32_t r = 0; r < kPullReasons && o < int(sizeof buf) - 48; ++r)
            o += std::snprintf(buf + o, sizeof buf - size_t(o), "%s%s %llu", r ? ", " : "", pullReasonName(r),
                               (unsigned long long)pullWhy_[r].load(std::memory_order_relaxed));
          logInfo("vulkan pull reasons (quads, cumulative): %s", buf);
          if (subCellCensusOn()) {  // Phase 4 C: sub-cell tuple census (how big a LUT would need to be)
            SubCellCensus& sc = subCellCensus(); std::vector<uint64_t> cnt; size_t rawN = 0;
            { std::lock_guard<std::mutex> lk(sc.mu); for (auto& kv : sc.tuples) cnt.push_back(kv.second); rawN = sc.raw.size(); }
            std::sort(cnt.begin(), cnt.end(), std::greater<uint64_t>());
            uint64_t tot = 0; for (uint64_t x : cnt) tot += x;
            auto cov = [&](size_t k) { uint64_t s = 0; for (size_t i = 0; i < k && i < cnt.size(); ++i) s += cnt[i]; return tot ? 100.0 * double(s) / double(tot) : 0.0; };
            size_t n90 = 0; { uint64_t s = 0; while (n90 < cnt.size() && double(s) < 0.9 * double(tot)) s += cnt[n90++]; }
            logInfo("vulkan sub-cell census: quads %llu, unique tuples %zu (raw corner tuples %zu), top16 %.1f%%, top64 %.1f%%, top256 %.1f%%, tuples for 90%% %zu",
                    (unsigned long long)tot, cnt.size(), rawN, cov(16), cov(64), cov(256), n90);
          }
        }
      }
#endif
      if (pull_) logInfo("vulkan pull: pulled quads/frame %.0f, packed12 fallback quads/frame %.0f, bucket-culled quads/frame %.0f (%s)",
                         double(perf_.pulledQuads) / n, double(perf_.fallbackQuads) / n, double(perf_.culledQuads) / n,
                         buckets_ ? "Phase 3 buckets on" : "buckets off");  // Mega-Pipeline Phase 2/3
      logInfo("vulkan upload split: %.1f meshes/frame %.2f MB/frame: arena alloc %.3f ms/frame, copy %.3f ms/frame (%s), blocks %zu; "
              "worker-direct published %.1f/frame (prepared total %llu)",
              double(perf_.uploads) / n, double(perf_.uploadBytes) / n / 1048576.0, perf_.allocMs / n, perf_.copyMs / n,
              cVertexArena::kindName(arena_->kind()), arena_->blockCount(), double(perf_.published) / n,
              (unsigned long long)directPrepared_.load());  // [C53-1] [C53-2]
      perf_ = Perf{};
    }
  }

  /// Records the swapchain-image -> host-staging copy for a screenshot grab.
  /// Runs inside the armed frame's command buffer, before present.
  void recordGrab(VkCommandBuffer cmd, uint32_t slot, uint32_t image) {
    grabReady_ = false;
    if (!swapchain_ || !swapchain_->supportsTransferSrc()) return;
    const VkExtent2D extent = swapchain_->extent();
    const VkFormat fmt = surface_.color.format;
    // chooseSurfaceConfig() only ever picks one of these two UNORM formats.
    if (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_UNORM) return;
    const VkDeviceSize need = VkDeviceSize(extent.width) * VkDeviceSize(extent.height) * 4;
    if (!grabStaging_ || grabStaging_->size() < need || grabExtent_.width != extent.width ||
        grabExtent_.height != extent.height) {
      const auto hostVisible =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      grabStaging_ =
          Buffer::create(*dev_, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostVisible);
      if (!grabStaging_) return;
      grabExtent_ = extent;
    }
    grabFormat_ = fmt;
    grabSlot_ = slot;
    const VkImage img = swapchain_->image(image);
    auto bar = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy cp{};  // no sType: plain copy description
    cp.bufferOffset = 0;
    cp.bufferRowLength = 0;  // tightly packed
    cp.bufferImageHeight = 0;
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageOffset = {0, 0, 0};
    cp.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           grabStaging_->handle(), 1, &cp);
    {  // F56-3 (Cortana 2026-10-17, review): make the copy visible to the host read in grabFramebuffer()
      auto hb = make<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
      hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      hb.buffer = grabStaging_->handle();
      hb.offset = 0;
      hb.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    }
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = 0;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &bar);
    grabReady_ = true;  // the slot's in-flight fence tells grabFramebuffer() when done
  }

  // JD Vulkan push: glow -> A (horizontal) -> B (vertical) -> A -> B; the composite samples B.
  void drawBloomBlur(VkCommandBuffer cmd) {
    const VkExtent2D be = swapchain_->blurExtent();
    const VkViewport vp{0, 0, float(be.width), float(be.height), 0.f, 1.f};
    const VkRect2D sc{{0, 0}, be};
    const float sx = 1.f / float(be.width), sy = 1.f / float(be.height);
    struct Step { int src, dst; float dx, dy; };  // src: 0 glow, 1 A, 2 B (postSets_ index); dst: 0 A, 1 B
    const Step steps[4] = {{0, 0, sx, 0.f}, {1, 1, 0.f, sy}, {2, 0, sx * 1.5f, 0.f}, {1, 1, 0.f, sy * 1.5f}};
    for (const Step& s : steps) {
      auto bp = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
      bp.renderPass = pipelines_->blurPass();
      bp.framebuffer = swapchain_->blurFramebuffer(s.dst);
      bp.renderArea = sc;
      vkCmdBeginRenderPass(cmd, &bp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdSetViewport(cmd, 0, 1, &vp);
      vkCmdSetScissor(cmd, 0, 1, &sc);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->blur());
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->postLayout(), 0, 1, &postSets_[s.src], 0, nullptr);
      const PostPush pp{{s.dx, s.dy, 1.f, 0.f}};
      vkCmdPushConstants(cmd, pipelines_->postLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
    }
  }

  // JD Vulkan push: procedural sparks, 6 vertices each, no buffers (vk/particle.vert).
  void drawParticles(VkCommandBuffer cmd, const FrameParams& p) {
    if (!(p.vkFx & FrameParams::kVkFxParticles) || p.vkParticleCount <= 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->particles());
    vkCmdDraw(cmd, uint32_t(std::min(p.vkParticleCount, 65536)) * 6u, 1, 0, 0);
    ++stats_.drawn;
  }

  void push(VkCommandBuffer cmd, const void* data, uint32_t bytes) const {
    vkCmdPushConstants(cmd, pipelines_->layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, data);
  }

  // 0.0.1.52 [A52-3] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): per-pass bind/push tracking.
  struct DrawState {
    VkBuffer bound = VK_NULL_HANDLE;
    float column[16]{};
    bool haveColumn = false;
  };
  static_assert(192 % sizeof(PackedVertex) == 0 && 192 % sizeof(GpuVertex12) == 0 && 64 % sizeof(PackedVertex) == 0,
                "arena slice alignment must be whole vertices (baseVertex)");  // 0.0.1.59 [P59-1]
  void drawMesh(VkCommandBuffer cmd, DrawState& ds, VkBuffer buffer, const Mesh& mesh, float pass) {
    if (buffer != ds.bound) {
      const VkDeviceSize zero = 0;
      vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &zero);
      ds.bound = buffer;
      ++perf_.binds;
    }
    const ChunkPush pc{{mesh.cx * 16.f, mesh.sy * 16.f, mesh.cz * 16.f}, pass,
                       {mesh.pulseP0[0], mesh.pulseP0[1], mesh.pulseP0[2], mesh.pulseP0[3]},
                       {mesh.pulseP1[0], mesh.pulseP1[1], mesh.pulseP1[2], mesh.pulseP1[3]},
                       {mesh.traceP0[0], mesh.traceP0[1], mesh.traceP0[2], mesh.traceP0[3]},
                       {mesh.traceP1[0], mesh.traceP1[1], mesh.traceP1[2], mesh.traceP1[3]}};
    static_assert(sizeof(ChunkPush) == 80 && offsetof(ChunkPush, pulseP0) == 16);
    if (ds.haveColumn && std::memcmp(ds.column, pc.pulseP0, sizeof ds.column) == 0) {
      push(cmd, &pc, 16);  // fast path: offset + pass only
      perf_.pushBytes += 16;
    } else {
      push(cmd, &pc, sizeof pc);
      std::memcpy(ds.column, pc.pulseP0, sizeof ds.column);
      ds.haveColumn = true;
      perf_.pushBytes += sizeof pc;
    }
    assert(mesh.slice.offset % vertexStride_ == 0);
    const int32_t baseVertex = int32_t(mesh.slice.offset / vertexStride_);  // 0.0.1.59 [P59-1]: 12 or 16
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, baseVertex, 0);
    ++perf_.apiCalls;  // mega-draw V2
    ++stats_.drawn;
    ++perf_.draws;
  }

  // Mega-draw V2 (Direction JD, design Archie, implementation Cortana): grows the slot's per-draw record and
  // indirect command buffers to hold `n` draws. The slot's fence was waited on, so the old buffers are idle.
  bool ensureMega(FrameSlot& f, size_t n) {
    if (f.megaDraws && f.megaCmds && f.megaCapacity >= n) return true;
    const VkDeviceSize cap = std::max<VkDeviceSize>(VkDeviceSize(n) * 2, 4096);
    auto d = Buffer::create(*dev_, cap * sizeof(ChunkPush), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto c = Buffer::create(*dev_, cap * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!d || !c) {
      static bool warned = false;
      if (!warned) { warned = true; logError("vulkan: mega-draw buffer allocation failed; using the push-constant path"); }
      return false;
    }
    f.megaDraws = std::move(d);
    f.megaCmds = std::move(c);
    f.megaCapacity = cap;
    // V2b: point binding 5 at the new record buffer. Called before this slot's set is bound (record()).
    const VkDescriptorBufferInfo info{f.megaDraws->handle(), 0, VK_WHOLE_SIZE};
    auto w = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = f.set;
    w.dstBinding = kBindingMegaDraws;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);
    return true;
  }

  // Mega-Pipeline Phase 2: records + commands per mesh (opaque + up to 6 pulled buckets + blend).
  size_t megaPerMesh() const { return pull_ ? 8 : 2; }
  // Phase 2: binding 6 slot b = arena block b (unused slots: the record buffer as a harmless placeholder).
  // The slot's fence was waited on, so rewriting its set is safe.
  void updateArenaSet(FrameSlot& f) {
    VkDescriptorBufferInfo info[kMaxPullBlocks];
    bool changed = false;
    const size_t blocks = std::min<size_t>(arena_->blockCount(), kMaxPullBlocks);
    for (uint32_t b = 0; b < kMaxPullBlocks; ++b) {
      const VkBuffer buf = b < blocks ? arena_->blockBuffer(b) : f.megaDraws->handle();
      info[b] = VkDescriptorBufferInfo{buf, 0, VK_WHOLE_SIZE};
      if (f.arenaSet[b] != buf) { f.arenaSet[b] = buf; changed = true; }
    }
    if (!changed) return;
    auto w = make<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    w.dstSet = f.set;
    w.dstBinding = kBindingArenaQuads;
    w.descriptorCount = kMaxPullBlocks;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = info;
    vkUpdateDescriptorSets(dev_->device(), 1, &w, 0, nullptr);
  }
  // Phase 2/3: draws the pulled quads of opaque records [0, n) (record index rides in firstInstance; one push of
  // uBlock and one multi-draw per arena-block run). Phase 3 (buckets_) skips face buckets facing away from the camera.
  void pullDraw(VkCommandBuffer cmd, FrameSlot& f, uint32_t n, const Vec3& cam) {
    auto* ic = reinterpret_cast<VkDrawIndexedIndirectCommand*>(f.megaCmds->mapped());
    uint32_t c = uint32_t(megaBuf_.size());  // commands go after the opaque records
    const uint32_t first = c;
    std::vector<uint32_t>& runBlock = pullRunBlock_;
    std::vector<uint32_t>& runStart = pullRunStart_;
    runBlock.clear(); runStart.clear();
    for (uint32_t r = 0; r < n; ++r) {
      const Mesh& m = *megaMesh_[r];
      const uint32_t total = m.pullBucket[6];
      if (!total) continue;
      if (runBlock.empty() || runBlock.back() != m.slice.block) { runBlock.push_back(m.slice.block); runStart.push_back(c); }
      if (!buckets_) {
        ic[c++] = VkDrawIndexedIndirectCommand{total * 6, 1, 0, int32_t(m.pullFirst * 4), r};
        perf_.pulledQuads += total;
        continue;
      }
      const float mn[3] = {m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
      const float cp[3] = {cam.x, cam.y, cam.z};
      uint32_t open = UINT32_MAX;  // merges adjacent visible buckets into one command
      for (uint32_t face = 0; face < 6; ++face) {
        const uint32_t ax = face >> 1;
        const bool vis = (face & 1u) ? cp[ax] < mn[ax] + 16.f : cp[ax] > mn[ax];  // +face needs cam > min, -face cam < max
        const uint32_t b0 = m.pullBucket[face], b1 = face < 5 ? m.pullBucket[face + 1] : total;
        if (vis && b1 > b0) {
          if (open == UINT32_MAX) open = b0;
          perf_.pulledQuads += b1 - b0;
        } else if (b1 > b0) {
          if (open != UINT32_MAX) { ic[c++] = VkDrawIndexedIndirectCommand{(b0 - open) * 6, 1, 0, int32_t((m.pullFirst + open) * 4), r}; open = UINT32_MAX; }
          perf_.culledQuads += b1 - b0;
        }
      }
      if (open != UINT32_MAX) ic[c++] = VkDrawIndexedIndirectCommand{(total - open) * 6, 1, 0, int32_t((m.pullFirst + open) * 4), r};
    }
    if (c == first) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->chunkPull());
    constexpr uint32_t kStride = sizeof(VkDrawIndexedIndirectCommand);
    runStart.push_back(c);
    for (size_t k = 0; k + 1 < runStart.size(); ++k) {
      const uint32_t block = runBlock[k];
      vkCmdPushConstants(cmd, pipelines_->layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof block, &block);
      for (uint32_t s = runStart[k]; s < runStart[k + 1];) {
        const uint32_t cnt = dev_->multiDrawIndirect() ? std::min(runStart[k + 1] - s, 65535u) : 1u;
        vkCmdDrawIndexedIndirect(cmd, f.megaCmds->handle(), VkDeviceSize(s) * kStride, cnt, kStride);
        ++perf_.apiCalls;
        s += cnt;
      }
    }
    pullCmds_ = c - first;
  }
  std::vector<uint32_t> pullRunBlock_, pullRunStart_;
  uint32_t pullCmds_ = 0;

  // Appends one draw record + its indexed command (firstInstance = record index).
  uint32_t megaPut(FrameSlot& f, VkBuffer buffer, const Mesh& m, float pass) {
    const uint32_t i = uint32_t(megaBuf_.size());
    auto* rec = reinterpret_cast<ChunkPush*>(f.megaDraws->mapped()) + i;
    *rec = ChunkPush{{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f}, pass,
                     {m.pulseP0[0], m.pulseP0[1], m.pulseP0[2], m.pulseP0[3]},
                     {m.pulseP1[0], m.pulseP1[1], m.pulseP1[2], m.pulseP1[3]},
                     {m.traceP0[0], m.traceP0[1], m.traceP0[2], m.traceP0[3]},
                     {m.traceP1[0], m.traceP1[1], m.traceP1[2], m.traceP1[3]}};
    assert(m.slice.offset % vertexStride_ == 0);
    reinterpret_cast<VkDrawIndexedIndirectCommand*>(f.megaCmds->mapped())[i] =
        VkDrawIndexedIndirectCommand{m.indexCount, 1, 0, int32_t(m.slice.offset / vertexStride_), i};
    megaBuf_.push_back(buffer);
    ++stats_.drawn;
    ++perf_.draws;
    return i;
  }

  // Issues records [begin, end): consecutive records on the same arena block share one vertex-buffer bind.
  // Order is kept, so the far -> near blend order survives.
  void megaDraw(VkCommandBuffer cmd, FrameSlot& f, uint32_t begin, uint32_t end, VkBuffer& bound) {
    const auto* ic = reinterpret_cast<const VkDrawIndexedIndirectCommand*>(f.megaCmds->mapped());
    for (uint32_t i = begin; i < end;) {
      uint32_t j = i + 1;
      while (j < end && megaBuf_[j] == megaBuf_[i]) ++j;
      if (megaBuf_[i] != bound) {
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &megaBuf_[i], &zero);
        bound = megaBuf_[i];
        ++perf_.binds;
      }
      // Mega-draw V3 (Direction JD, design Archie, implementation Cortana): LL_VK_MEGA=2 issues the run as one
      // vkCmdDrawIndexedIndirect (multiDrawIndirect) or one single-command indirect call per draw without it.
      // Needs drawIndirectFirstInstance (the record index rides in firstInstance); otherwise V2 direct draws.
      if (megaMode_ >= 2 && dev_->drawIndirectFirstInstance()) {
        constexpr uint32_t kStride = sizeof(VkDrawIndexedIndirectCommand);
        if (dev_->multiDrawIndirect()) {
          for (uint32_t k = i; k < j; k += 65535u) {
            vkCmdDrawIndexedIndirect(cmd, f.megaCmds->handle(), VkDeviceSize(k) * kStride, std::min(j - k, 65535u), kStride);
            ++perf_.apiCalls;
          }
        } else {
          for (uint32_t k = i; k < j; ++k) {
            vkCmdDrawIndexedIndirect(cmd, f.megaCmds->handle(), VkDeviceSize(k) * kStride, 1, kStride);
            ++perf_.apiCalls;
          }
        }
      } else {
        for (uint32_t k = i; k < j; ++k) {
          vkCmdDrawIndexed(cmd, ic[k].indexCount, 1, 0, ic[k].vertexOffset, ic[k].firstInstance);
          ++perf_.apiCalls;
        }
      }
      i = j;
    }
  }

  void drawChunks(VkCommandBuffer cmd, FrameSlot& f, const FrameParams& p) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->chunk());
    vkCmdBindIndexBuffer(cmd, quadIndices_->handle(), 0, VK_INDEX_TYPE_UINT16);  // 0.0.1.52 [A52-2]: uint16 quad indices
    stats_.drawn = 0;
    stats_.meshes = uint32_t(meshes_.size());
#if LL_STREAM_COUNTERS
    const auto tCull0 = std::chrono::steady_clock::now();  // Phase 4: cull / sort / record split
#endif
    if (fastCull_) {  // Phase 4.2a
      for (const CullEnt& e : cullList_) {
        const Vec3 mn{e.x, e.y, e.z};
#if LL_STREAM_COUNTERS
        ++perf_.cullTested;
#endif
        if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos, frustumMarginDeg_) != CullDecision::Full) {
#if LL_STREAM_COUNTERS
          ++perf_.cullFrustum;
#endif
          continue;
        }
        visible_.push_back({arena_->buffer(e.m->slice), e.m});
      }
    }
    for (const auto& [k, mesh] : meshes_) {
      if (fastCull_) break;
      const Vec3 mn{mesh.cx * 16.f, mesh.sy * 16.f, mesh.cz * 16.f};
      // Prototype pipeline: Full draws; MeshOnly/Cull skip (mesh stays uploaded).
#if LL_STREAM_COUNTERS
      ++perf_.cullTested;  // Phase 4.0: frustum only; this renderer has no occlusion pass (occlusion culled = 0)
#endif
      if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos, frustumMarginDeg_) !=
          CullDecision::Full) {
#if LL_STREAM_COUNTERS
        ++perf_.cullFrustum;
#endif
        continue;
      }
      visible_.push_back({arena_->buffer(mesh.slice), &mesh});
    }
    // 0.0.1.52 [A52-3] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): draw compaction. Visible meshes are grouped
    // by vertex buffer (arena block) and column; the buffer is bound once per block at offset 0 and
    // each draw passes baseVertex = slice.offset / sizeof(PackedVertex) (slices are 64-byte
    // aligned since A54-3a, a multiple of 16; Cortana 2026-10-16 2335 F55-4). The 64-byte column payload (pulse/trace) is pushed only when it
    // differs from the last draw's; otherwise only offset + pass (16 bytes) goes out.
#if LL_STREAM_COUNTERS
    const auto tCull1 = std::chrono::steady_clock::now();
#endif
    std::sort(visible_.begin(), visible_.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first < b.first;
      if (a.second->cx != b.second->cx) return a.second->cx < b.second->cx;
      return a.second->cz < b.second->cz;
    });
#if LL_STREAM_COUNTERS
    const auto tCull2 = std::chrono::steady_clock::now();
    perf_.cullMs += std::chrono::duration<double, std::milli>(tCull1 - tCull0).count();
    perf_.sortMs += std::chrono::duration<double, std::milli>(tCull2 - tCull1).count();
#endif
    // Mega-draw V2 (Direction JD, design Archie, implementation Cortana): LL_VK_MEGA=1 moves the per-draw values
    // into the slot's instance buffer; the same order, culling and blend pass as below.
    // V2b (LL_VK_MEGA=3): the storage-buffer pipelines; needs drawIndirectFirstInstance (index in firstInstance).
    const bool ssbo = megaMode_ == 3 && dev_->drawIndirectFirstInstance();
    const VkPipeline megaOpaque = ssbo ? pipelines_->chunkSsbo(megaProbe_) : pipelines_->chunkMega();
    const VkPipeline megaBlend = ssbo ? pipelines_->chunkBlendSsbo(megaProbe_) : pipelines_->chunkBlendMega();
    if (megaMode_ > 0 && megaOpaque && megaBlend && f.megaDraws && f.megaCmds && f.megaCapacity >= visible_.size() * megaPerMesh()) {
      const bool pulling = pull_ && ssbo;  // Mega-Pipeline Phase 2
      megaMesh_.clear();
      megaBuf_.clear();
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, megaOpaque);
      if (!ssbo) {
        const VkBuffer draws = f.megaDraws->handle();
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 1, 1, &draws, &zero);
      }
      for (const auto& [buffer, mp] : visible_) {
        const Mesh& mesh = *mp;
        megaPut(f, buffer, mesh, 0.f);
        if (pulling) { megaMesh_.push_back(&mesh); perf_.fallbackQuads += mesh.indexCount / 6; }
        if (mesh.blend) {
          const float dx = mesh.cx * 16.f + 8.f - p.camPos.x, dy = mesh.sy * 16.f + 8.f - p.camPos.y,
                      dz = mesh.cz * 16.f + 8.f - p.camPos.z;
          blendQueue_.push_back({dx * dx + dy * dy + dz * dz, &mesh});
        }
      }
      visible_.clear();
      VkBuffer bound = VK_NULL_HANDLE;
      const uint32_t opaque = uint32_t(megaBuf_.size());
      megaDraw(cmd, f, 0, opaque, bound);
      pullCmds_ = 0;
      if (pulling) pullDraw(cmd, f, opaque, p.camPos);  // Phase 2/3: packed quads, same records (pass 0)
#if LL_STREAM_COUNTERS
      const auto tB0 = std::chrono::steady_clock::now();
      perf_.opaqueMs += std::chrono::duration<double, std::milli>(tB0 - tCull2).count();
      perf_.blendDraws += blendQueue_.size();
#endif
      if ((p.vkFx & FrameParams::kVkFxBulbBlend) && !blendQueue_.empty()) {
        std::sort(blendQueue_.begin(), blendQueue_.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        // Phase 2: blend records start after the pulled commands (slots [opaque, opaque + pullCmds_) are taken).
        for (uint32_t k = 0; k < pullCmds_; ++k) megaBuf_.push_back(VK_NULL_HANDLE);
        const uint32_t blendBegin = uint32_t(megaBuf_.size());
        for (const auto& [d2, mp] : blendQueue_) megaPut(f, arena_->buffer(mp->slice), *mp, 1.f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, megaBlend);
        if (pullCmds_) bound = VK_NULL_HANDLE;  // the pull pipeline has no vertex input; rebind
        megaDraw(cmd, f, blendBegin, uint32_t(megaBuf_.size()), bound);
      }
      blendQueue_.clear();
#if LL_STREAM_COUNTERS
      perf_.blendMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tB0).count();
#endif
      return;
    }
    DrawState ds;
    for (const auto& [buffer, mp] : visible_) {
      const Mesh& mesh = *mp;
      const Vec3 mn{mesh.cx * 16.f, mesh.sy * 16.f, mesh.cz * 16.f};
      drawMesh(cmd, ds, buffer, mesh, 0.f);
      if (mesh.blend) {
        const float dx = mn.x + 8.f - p.camPos.x, dy = mn.y + 8.f - p.camPos.y, dz = mn.z + 8.f - p.camPos.z;
        blendQueue_.push_back({dx * dx + dy * dy + dz * dz, &mesh});
      }
    }
    visible_.clear();
    // JD Vulkan push (Cortana 2026-10-16): blended bulb pass. Meshes holding alpha-blend faces
    // draw again far -> near with blending on and depth writes off; the shader keeps only the
    // blend faces here (uPass 1) and drops them from the opaque pass above.
    if ((p.vkFx & FrameParams::kVkFxBulbBlend) && !blendQueue_.empty()) {
      std::sort(blendQueue_.begin(), blendQueue_.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->chunkBlend());
      for (const auto& [d2, mp] : blendQueue_) drawMesh(cmd, ds, arena_->buffer(mp->slice), *mp, 1.f);  // [A52-3]
    }
    blendQueue_.clear();
  }

  void drawEntities(VkCommandBuffer cmd, FrameSlot& f) {
    if (entityVerts_.empty()) return;
    if (!f.entities || f.entityCapacityVerts < entityVerts_.size()) {
      const VkDeviceSize verts = std::max<VkDeviceSize>(entityVerts_.size() * 2, 4096);
      auto buf = Buffer::create(*dev_, verts * sizeof(PackedVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!buf) {
        static bool warned = false;
        if (!warned) { warned = true; logError("vulkan: entity vertex buffer allocation failed; entities skipped"); }
        return;
      }
      f.entities = std::move(buf);  // the slot's fence was waited on: the old buffer is idle
      f.entityCapacityVerts = verts;
    }
    std::memcpy(f.entities->mapped(), entityVerts_.data(), entityVerts_.size() * sizeof(PackedVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->entity());
    const ChunkPush pc{{entityOrigin_[0], entityOrigin_[1], entityOrigin_[2]}, 0.f, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    push(cmd, &pc, sizeof pc);
    const VkBuffer buffer = f.entities->handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    vkCmdDraw(cmd, uint32_t(entityVerts_.size()), 1, 0, 0);
    ++stats_.drawn;
  }

  // (Re)uploads the sprite atlas pages as one texture array (layer = page
  // index, pages upload row-first exactly as GL33 uploads them). All pages
  // must share one size — the sprite registry's pages always do (square,
  // power-of-two, one page size per registry); a mixed-size span is the
  // one genuinely unsupported sub-case and is named, warned once, and the
  // frame's sprites are skipped rather than misdrawn.
  bool uploadSpritePages(std::span<const ::litholattice::Image> pages, uint64_t version) {
    uint32_t w = 1, h = 1;
    size_t layers = 1;
    if (!pages.empty()) {
      w = uint32_t(pages[0].width());
      h = uint32_t(pages[0].height());
      layers = pages.size();
      for (const ::litholattice::Image& page : pages) {
        if (page.width() != int(w) || page.height() != int(h) || w == 0 || h == 0) {
          static bool warnedMixed = false;
          if (!warnedMixed) {
            warnedMixed = true;
            logWarn("vulkan: sprite atlas pages have mixed sizes — sprite pass skipped for this atlas (pages must share one size)");
          }
          spriteAtlasPages_ = 0;
          spriteAtlasUploaded_ = version;
          return false;
        }
      }
    }
    vkDeviceWaitIdle(dev_->device());
    const VkDeviceSize layerBytes = VkDeviceSize(w) * h * 4;
    auto staging = Buffer::create(*dev_, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!staging) return false;
    if (pages.empty()) {
      std::memset(staging->mapped(), 0, size_t(layerBytes));  // dummy: transparent black
    } else {
      for (size_t i = 0; i < layers; ++i)
        std::memcpy(staging->mapped() + layerBytes * i, pages[i].pixels().data(), size_t(layerBytes));
    }
    auto ci = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = uint32_t(layers);
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto image = vk::Image::create(*dev_, ci, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!image) return false;
    const VkImage img = image->handle();
    const VkBuffer src = staging->handle();
    const bool ok = submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer cmd) {
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, uint32_t(layers)};
      region.imageExtent = {w, h, 1};
      vkCmdCopyBufferToImage(cmd, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    if (!ok) return false;
    spriteAtlas_ = std::move(image);
    spriteAtlasPages_ = pages.size();
    spriteAtlasUploaded_ = version;
    if (frames_[0].set != VK_NULL_HANDLE) writeDescriptors();
    return true;
  }

  void uploadSpriteAtlas(const FrameParams& p) {
    if (!uploadSpritePages(p.spriteAtlasPages, p.spriteAtlasVersion))
      logError("vulkan: sprite atlas upload failed; sprites skipped for atlas version %llu",
               (unsigned long long)p.spriteAtlasVersion);
  }

  // Sprite pass (JD wiring 2026-10-13), at parity with GL33: the frame's
  // quads expand to vertices in INPUT ORDER (blend/additive arrive
  // pre-sorted far-to-near; never re-sorted), stream into this slot's
  // grow-only vertex buffer, and draw in runs of equal (shade, page):
  // cutout = alpha test 0.5 + depth write, blend = SRC_ALPHA /
  // ONE_MINUS_SRC_ALPHA, additive = SRC_ALPHA / ONE — the three sprite
  // pipelines. Double-sided, like GL33 (sprites read from both sides).
  void drawSprites(VkCommandBuffer cmd, FrameSlot& f, const FrameParams& p) {
    if (!p.spriteQuads || p.spriteQuadCount == 0 || spriteAtlasPages_ == 0) return;
    vk_sprite::buildSpriteVertices(p.spriteQuads, p.spriteQuadCount, spriteVerts_);
    if (spriteVerts_.empty()) return;
    if (!f.sprites || f.spriteCapacityVerts < spriteVerts_.size()) {
      const VkDeviceSize verts = std::max<VkDeviceSize>(spriteVerts_.size(), 6144);
      auto buf = Buffer::create(*dev_, verts * sizeof(vk_sprite::SpriteVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!buf) {
        logError("vulkan: sprite vertex buffer allocation failed; sprites skipped this frame");
        return;
      }
      f.sprites = std::move(buf);
      f.spriteCapacityVerts = verts;
    }
    std::memcpy(f.sprites->mapped(), spriteVerts_.data(),
                spriteVerts_.size() * sizeof(vk_sprite::SpriteVertex));
    const VkBuffer buffer = f.sprites->handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    const VkPipeline pipelines[3] = {pipelines_->spriteCutout(), pipelines_->spriteBlend(),
                                     pipelines_->spriteAdditive()};
    bool drew = false;
    size_t skippedQuads = 0;
    for (const vk_sprite::SpriteRun& run :
         vk_sprite::spriteRuns(p.spriteQuads, p.spriteQuadCount, spriteAtlasPages_)) {
      if (run.pipeline < 0) {  // non-sprite shade or page out of range: skip, never misdraw
        skippedQuads += run.vertexCount / 6;
        continue;
      }
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[run.pipeline]);
      const SpritePush pc{vk_sprite::spriteAlphaTestForShade(run.shade), {0.f, 0.f, 0.f}};
      vkCmdPushConstants(cmd, pipelines_->layout(),
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
      vkCmdDraw(cmd, run.vertexCount, 1, run.firstVertex, 0);
      drew = true;
    }
    if (skippedQuads > 0) {
      static bool warnedSkip = false;
      if (!warnedSkip) {
        warnedSkip = true;
        logWarn("vulkan: sprite pass skipped %zu quads (non-sprite shade or atlas page out of range)", skippedQuads);
      }
    }
    if (drew) ++stats_.drawn;
  }

  // Block outline (depth tested), crosshair (always on top), then the HUD, from this frame's
  // overlay buffer: lines at offset 0, HUD vertices at kHudOffset.
  void drawOverlay(VkCommandBuffer cmd, const FrameSlot& f, const FrameParams& p) {
    const VkBuffer buffer = f.overlay->handle();
    uint8_t* const base = f.overlay->mapped();
    VkDeviceSize offset = 0;
    auto drawLines = [&](VkPipeline pipeline, std::span<const Vec3> pts, const Mat4& m, Vec3 color) {
      std::memcpy(base + offset, pts.data(), pts.size_bytes());
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      LinePush pc{};
      std::memcpy(pc.matrix, m.m, sizeof pc.matrix);
      pc.color[0] = color.x; pc.color[1] = color.y; pc.color[2] = color.z; pc.color[3] = 1.f;
      push(cmd, &pc, sizeof pc);
      vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
      vkCmdDraw(cmd, uint32_t(pts.size()), 1, 0, 0);
      offset += pts.size_bytes();
    };
    if (p.highlight)
      drawLines(pipelines_->lineDepth(), blockOutlineLines(p.hx, p.hy, p.hz), p.viewProj, {0.05f, 0.05f, 0.08f});
    if (p.crosshair) drawLines(pipelines_->lineOverlay(), crosshairLines(p.width, p.height), Mat4::identity(), {1, 1, 1});

    buildHudVertices(p.hud, hudVerts_);
    const size_t maxVerts = size_t((kOverlayBytes - kHudOffset) / sizeof(HudVertex));
    const size_t count = std::min(hudVerts_.size(), maxVerts / 6 * 6);
    // D-M6: the overlay buffer is fixed-size; excess HUD verts are dropped.
    // Loud once instead of silent truncation.
    if (count < hudVerts_.size()) {
      static bool warnedHudTrunc = false;
      if (!warnedHudTrunc) { warnedHudTrunc = true; logWarn("vulkan: HUD overlay buffer full — %zu of %zu HUD vertices dropped", hudVerts_.size() - count, hudVerts_.size()); }
    }
    if (count == 0) return;
    std::memcpy(base + kHudOffset, hudVerts_.data(), count * sizeof(HudVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->hud());
    const HudPush pc{{float(p.width), float(p.height)}};
    push(cmd, &pc, sizeof pc);
    const VkDeviceSize hudOffset = kHudOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &hudOffset);
    vkCmdDraw(cmd, uint32_t(count), 1, 0, 0);
  }

  // Declaration order is destruction order reversed: the device goes last.
  std::unique_ptr<cVulkanDevice> dev_;
  UniqueCommandPool pool_;
  SurfaceConfig surface_;
  std::unique_ptr<RenderPipelines> pipelines_;
  std::unique_ptr<cVertexArena> arena_;
  std::optional<Buffer> quadIndices_;
  std::optional<vk::Image> font_;
  UniqueSampler fontSampler_;
  // Sprite pass state (JD wiring 2026-10-13): atlas pages as one texture
  // array + its sampler; spriteAtlasUploaded_ is the registry version on
  // the GPU (UINT64_MAX = nothing real uploaded yet, dummy array bound).
  std::optional<vk::Image> spriteAtlas_;
  UniqueSampler spriteSampler_;
  uint64_t spriteAtlasUploaded_ = UINT64_MAX;
  size_t spriteAtlasPages_ = 0;
  std::vector<vk_sprite::SpriteVertex> spriteVerts_;
  std::array<FrameSlot, kFramesInFlight> frames_;
  UniqueDescriptorPool descriptorPool_;
  std::unique_ptr<cVkTextureArray> textures_;
  std::unique_ptr<cSwapchain> swapchain_;
  std::vector<UniqueSemaphore> renderDone_;  // per swapchain image
  VkExtent2D requested_{};    // window size the swapchain was built for
  bool swapchainStale_ = false;  // out of date / suboptimal: rebuild before the next frame
  float frustumMarginDeg_ = 5.f;  ///< FOV culling margin, degrees (JD 2026-10-11); game sets it
  CullProtoState cullProto_;  ///< culling-cache prototypes (JD 2026-10-11); all default off
  std::unordered_map<int64_t, Mesh> meshes_;
  // Phase 4.2a (LL_VK_FASTCULL=1; Direction JD, LithoPipeline designed by Archie, implemented by Cortana): a compact
  // array of chunk origins + mesh pointers (map nodes are pointer-stable), so the per-frame frustum loop walks
  // 16-byte entries instead of hash-map nodes. Same decide() call, same decisions; maintained always, used on the flag.
  struct CullEnt { float x, y, z; Mesh* m; };
  std::vector<CullEnt> cullList_;
  bool fastCull_ = false;
  void cullAdd(Mesh& m) { m.cullIdx = uint32_t(cullList_.size()); cullList_.push_back({m.cx * 16.f, m.sy * 16.f, m.cz * 16.f, &m}); }
  void cullRemove(Mesh& m) {
    if (m.cullIdx >= cullList_.size()) return;
    const uint32_t i = m.cullIdx;
    cullList_[i] = cullList_.back();
    cullList_[i].m->cullIdx = i;
    cullList_.pop_back();
    m.cullIdx = UINT32_MAX;
  }
  std::vector<std::pair<float, const Mesh*>> blendQueue_;  ///< JD Vulkan push: this frame's blend meshes
  UniqueDescriptorPool postPool_;                          ///< JD Vulkan push: bloom sets
  VkDescriptorSet postSets_[3]{};                          ///< glow, blur A, blur B
  std::vector<Retired> retired_;
  uint64_t frameNumber_ = 0;
  // Screenshot grab state (lithoscript `screenshot`): armed by requestGrab()
  // during update; recordGrab() copies the armed frame's swapchain image to
  // grabStaging_ before present; grabFramebuffer() waits on the armed slot's
  // fence and swizzles to top-first RGB.
  bool grabArmed_ = false;
  bool grabReady_ = false;
  uint32_t grabSlot_ = 0;
  VkFormat grabFormat_ = VK_FORMAT_UNDEFINED;
  VkExtent2D grabExtent_{0, 0};
  std::optional<Buffer> grabStaging_;
  FrameUniforms materials_{};
  std::vector<HudVertex> hudVerts_;
  std::vector<PackedVertex> entityVerts_;  ///< Cortana 2026-10-16: this frame's entity batch
  float entityOrigin_[3] = {0.f, 0.f, 0.f};
  RenderStats stats_;
  // 0.0.1.52 [A52-3] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): draw compaction + perf counters (logged every 600 frames).
  std::vector<std::pair<VkBuffer, const Mesh*>> visible_;
  std::vector<VkBuffer> megaBuf_;  ///< mega-draw V2: arena block of each record this frame
  struct Parked { VkBuffer dst; VkDeviceSize offset; std::vector<uint8_t> data; };  // [A52-4]
  std::vector<Parked> parked_;
  struct Perf {
    uint64_t frames = 0, draws = 0, binds = 0, pushBytes = 0, gpuFrames = 0, uploads = 0, uploadBytes = 0;
    double allocMs = 0, copyMs = 0;  // [C53-1]
    uint64_t published = 0;  // [C53-2]
    double gpuMs = 0, gpuMaxMs = 0;
    uint64_t evictions = 0, remeshes = 0, cullTested = 0, cullFrustum = 0;  ///< Phase 4.0 (LL_STREAM_COUNTERS)
    double cullMs = 0, sortMs = 0, opaqueMs = 0, blendMs = 0;  ///< Phase 4: chunk-pass CPU split (mega path)
    uint64_t blendDraws = 0;
    uint64_t pulledQuads = 0, fallbackQuads = 0, culledQuads = 0;  ///< Mega-Pipeline Phase 2/3 (quads drawn / culled per log window)
    uint64_t apiCalls = 0;   ///< mega-draw V2: chunk draw API calls (vkCmdDrawIndexed / vkCmdDrawIndexedIndirect)
    double chunkCpuMs = 0;
    double chunkGpuMs = 0;   ///< V2b: GPU time between the chunk-pass timestamps   ///< mega-draw V2: CPU time recording the chunk passes
  } perf_;
  float timestampPeriodNs_ = 1.f;
};

}  // namespace

std::unique_ptr<IRenderer> createVulkanRenderer(const VulkanRendererDesc& desc, std::string* error) {
  auto r = std::make_unique<cVulkanRenderer>();
  std::string err;
  if (!r->init(desc, &err)) {
    if (error) *error = err;
    return nullptr;
  }
  return r;
}

}  // namespace litholattice
