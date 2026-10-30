// Cortana 2026-10-15 1010-1112: RND-H4, GAME-H1, RND-MT
// LithoLattice software renderer: CPU rasterizer, no GPU required.
// Like Half-Life's software mode: projects PackedVertex data and rasterizes
// triangles directly to a framebuffer on CPU. For headless testing, low-end
// devices, and VMs without GPUs.
#pragma once
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "litholattice/render/cpu_texture_array.h"
#include "litholattice/render/cull_prototypes.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/software_shaders.h"
#include "litholattice/render/pbr_software_shaders.h"
#include "litholattice/render/sw_upload_ring.h"

namespace litholattice {

// AA ladder + scratch are defined in render/software_aa.h (which includes
// this header for SoftwareFramebuffer, hence the forward declarations).
enum class SoftwareAaMode : int;
struct SoftwareAaScratch;

/// CPU framebuffer: RGBA pixels, written by the software rasterizer.
struct SoftwareFramebuffer {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> pixels;  ///< RGBA8, row-major

  void resize(uint32_t w, uint32_t h) {
    width = w; height = h;
    pixels.assign(size_t(w) * h, 0xFF000000);  // opaque black
  }
  void clear(uint32_t color = 0xFF000000) {
    std::fill(pixels.begin(), pixels.end(), color);
  }
};

/// Software renderer: IRenderer implemented with CPU rasterization, so a
/// host holding an IRenderer can select it like the GL/Vulkan backends.
/// Two surfaces, one rasterizer (they agree pixel-for-pixel given
/// equivalent inputs):
///  - the IRenderer interface: chunk/entity mesh stores + renderFrame()
///    (sky clear, stored meshes at their (cx,sy,cz) origins, entity verts,
///    FrameParams sprite quads in the flat average-color form);
///  - the direct CPU surface (renderMesh / framebuffer / shading controls)
///    that renderFrame is built on and that headless consumers — MCP vision
///    captures, biome_capture — drive directly.
/// Shading: flat face colors by default; setPerPixelShading(true) runs the
/// tier-selected fragment shader (software_shaders.h basic tier, or the PBR
/// tier via setShadingTier/setPbrContext) sampling textureArray().
/// FrameParams overlays ARE drawn (P1-RENDER H3, 2026-10-13): the HUD layer
/// is rasterized from buildHudVertices() output (solid rects + font-atlas
/// glyphs, alpha-blended in paint order, mirroring the GL33 hud program),
/// plus the block highlight outline and the crosshair as CPU line draws.
/// The direct renderMesh surface stays world-only, so MCP-vision and
/// biome_capture frames (which drive renderMesh directly) are unchanged.
/// PBR (P1-RENDER H2, 2026-10-13): the full IRenderer PBR surface is
/// implemented — setPbrMaterials feeds an owned PbrFragmentContext,
/// uploadPbrChunkMesh stores BinaryQuad meshes whose per-vertex compact
/// material IDs shade through pbrFragmentShader, setPbrTier selects the
/// PBR tier, setAmbientMode feeds the context. The transparency-tier
/// ladder itself is NOT implemented on CPU (see pbr_software_shaders.h
/// ULTRA TIER GAP); setPbrTier logs that once. Frustum culling with the
/// render.frustum_margin_deg margin + the cull prototypes runs in
/// renderFrame, like GL/VK (P1-RENDER M1).
// Cortana 2026-10-15 1505 (ENGINE, audit F-6): THREADING CONTRACT: setters (textures, ground FX,
// mesh tables, sizes) are owning-thread only and only between frames; band threads read these members
// during renderFrame/renderMesh. A second view (e.g. MCP vision) must use its own instance.
class cSoftwareRenderer : public IRenderer {
 public:
  cSoftwareRenderer();
  ~cSoftwareRenderer() override;

  // --- IRenderer interface ---------------------------------------------------
  const char* name() const override { return "software"; }
  bool prefersGreedyMeshes() const override { return true; }  // Cortana 2026-10-15 1705: greedy meshing [ENGINE]
  std::string deviceInfo() const override;
  /// Frames land in framebuffer(); there is no window or swapchain.
  std::string presentMode() const override { return "framebuffer"; }
  ITextureArray& textureArray() override { return textures_; }
  /// Not consumed: the software fragment inputs carry no BlockMaterials —
  /// mask/palette/strip shading here resolves from the PackedVertex
  /// mode/value fields, and PBR material data arrives via setPbrContext().
  /// Accepted for interface parity (documented no-op).
  void setMaterials(const BlockMaterials& materials) override;
  /// No shader programs on the CPU path (fragment shading is compiled in),
  /// so no program can use pack sources: returns 0 by the contract.
  int loadShaders(const ShaderLibrary& shaders) override;
  void uploadChunkMesh(const ChunkMesh& mesh) override;  // replace by (cx,sy,cz); empty verts = remove
  /// Three-way software split (Design B, Stage 2): staged uploads. While
  /// the upload ring is enabled (setUploadRingEnabled, app-driven), these
  /// stage the mesh in the SPSC ring and the executor applies it at the
  /// frame seal via the unchanged entry points above; while disabled they
  /// apply immediately, exactly as the direct calls did. A false return
  /// means the ring is full and the mesh was NOT taken (never dropped).
  bool enqueueChunkMesh(ChunkMesh&& mesh) override;
  bool enqueuePbrChunkMesh(PbrChunkMesh&& mesh) override;
  bool enqueueClearChunkMeshes() override;
  void enqueuePbrMaterials(const cMaterialReg& reg) override;
  bool meshUploadsQueued() const override { return uploadRingEnabled_; }
  /// App-facing ring switch (per instance, unlike the pool-wide detached
  /// mode): set by the app alongside setDetachedPoolMode when the software
  /// threeway split is enabled; off (the default) is the pre-Stage-2
  /// renderer exactly. Only the game's display renderer is ever enabled —
  /// vision/capture/test instances keep applying uploads directly.
  void setUploadRingEnabled(bool on) { uploadRingEnabled_ = on; }
  /// Entries published to the ring so far: a frame job's drain target.
  uint64_t uploadRingPublished() const { return uploadRing_.published(); }
  /// Ring accounting for the shutdown/bench log: pushes accepted, pushes
  /// refused (ring full — the game's spill held those), peak occupancy,
  /// and the consumer side (drained / applied after coalescing / coalesced
  /// away / clears applied). pushed == drained must hold at rest.
  struct UploadRingStats {
    uint64_t pushed = 0, pushFull = 0, maxOccupancy = 0;
    uint64_t drained = 0, applied = 0, coalesced = 0, clears = 0;
  };
  UploadRingStats uploadRingStats() const;
  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override;  // dynamic triangle verts, replaced per upload
  void setEntityOrigin(float x, float y, float z) override { entityOrigin_[0] = x; entityOrigin_[1] = y; entityOrigin_[2] = z; }
  float entityOrigin_[3] = {0.f, 0.f, 0.f};  ///< GAME-H1: world origin of the entity batch
  // --- PBR / culling surface (P1-RENDER H2): real overrides, not no-ops ---
  /// True: BinaryQuad uploads shade through the CPU PBR fragment path.
  bool supportsPbrPath() const override { return true; }
  /// Feeds the owned PbrFragmentContext's registry (the context the PBR
  /// fragment path resolves compact material IDs through).
  void setPbrMaterials(const cMaterialReg& reg) override;
  /// Records the tier and selects the software PBR shading tier. The CPU
  /// path has no transparency-tier ladder (documented gap): PBR fragments
  /// shade opaque; logged once, not silent.
  void setPbrTier(PbrTransparencyTier t) override;
  /// Flood-fill ambient mode, applied to the owned PBR context.
  void setAmbientMode(AmbientMode m) override;
  /// Frustum-culling margin in degrees, clamped 0..15 like GL/VK; used by
  /// renderFrame's per-mesh culling (M1).
  void setFrustumMarginDeg(float degrees) override;
  /// Near-camera model dithering (JD 2026-10-13): real implementation —
  /// the discard runs in both rasterizers from the fragment's view depth
  /// (clip-w interpolated), pattern = model_dither.h IGN. Applies to chunk
  /// meshes, PBR meshes, entities and sprites; HUD/lines are never faded.
  /// The render.model_dither SETTING defaults on (JD's "standard engine
  /// staple" call) and the game applies it at startup; this class starts
  /// with the fade OFF so headless consumers (tests, capture tools) see
  /// the pre-dither output unless they opt in — the same contract the
  /// other renderer state follows.
  void setModelDither(bool enabled, float nearBlocks = 2.f, float farBlocks = 8.f) override;
  bool modelDither() const { return ditherEnabled_; }
  float modelDitherNear() const { return ditherNear_; }
  float modelDitherFar() const { return ditherFar_; }
  /// Anti-aliasing ladder (JD 2026-10-13, render/software_aa.h): named
  /// modes behind render.aa — "off" (default; JD picks the shipped
  /// default from the capture sets), "fxaa" (cheapest post filter),
  /// "cmaa" (flagship: conservative morphological coverage restoration),
  /// "ssaa2x" (2x supersampling, render-time: renderFrame rasterizes at
  /// 2x and box-resolves before the overlay/HUD stage). All presentational.
  void setAaMode(const char* modeName) override;
  const char* aaModeName() const;
  void setBloom(bool on) override { bloom_ = on; }  ///< Cortana 2026-10-16: render.bloom
  void setShimmer(bool on) override { shimmerOn_ = on; }  ///< Cortana 2026-10-16: render.shimmer
  void setMipmaps(bool on) override { mipOn_ = on; }
  void setEmissiveLight(bool on) override { emissiveLightOn_ = on; }  ///< Cortana 2026-10-16     ///< Cortana 2026-10-16: render.mipmaps (BUG-0005)
  void setAo(bool on) override;                          ///< Cortana 2026-10-16: render.ao (live)
  void setTexRes(int tile) override { textures_.setMaxTile(tile); }  ///< render.tex_res (next texture upload / F9)
  bool bloom() const { return bloom_; }
  /// Apply the current POST aa mode (fxaa / cmaa) to the framebuffer in
  /// place; Off and Ssaa2x are no-ops here (SSAA happens at render time).
  /// For direct-surface consumers (captures, MCP vision).
  void applyAaPostPass();
  /// ASCII pipeline (EXPERIMENT TIER, JD 2026-10-13,
  /// render/software_ascii.h): when enabled, renderFrame converts the
  /// finished world frame into a glyph visualization and stores the raw
  /// text grid (asciiText()) — the Daemon Sight low-end configuration.
  /// Driven by render.ascii / render.ascii_cols; default off.
  void setAsciiMode(bool enabled, int cols) override;
  void setAsciiGlyphSet(const char* setName) override;
  bool asciiEnabled() const { return asciiEnabled_; }
  int asciiCols() const { return asciiCols_; }
  int asciiGlyphSet() const { return asciiSet_; }  ///< AsciiGlyphSet as int (header cycle)
  /// Value span for the stage's luminance -> level mapping
  /// (render.ascii_value_span; JD amendment 2026-10-13: default 255 =
  /// the full 0-255 range, NOT a 64-step ramp).
  void setAsciiValueSpan(int span) override;
  int asciiValueSpan() const { return asciiSpan_; }
  /// Convert the current framebuffer to its ASCII glyph visualization
  /// and refresh asciiText(). Direct-surface sibling of the renderFrame
  /// ASCII stage.
  void applyAsciiPass();
  const std::string& asciiText() const { return asciiText_; }
  void setCullProtoConfig(const CullProtoConfig& cfg) override;
  CullProtoStats cullProtoStats() const override;
  /// BinaryQuad meshes, stored beside the classic meshes; empty = remove.
  void uploadPbrChunkMesh(const PbrChunkMesh& mesh) override;
  /// Clears the chunk mesh store. The entity verts are NOT cleared, matching
  /// the GL/Vulkan backends (their entity store is separate there too).
  void clearChunkMeshes() override;
  /// Three-way software split (Design B, Stage 0): the mesh-store seal. A
  /// monotonic generation bumped by every store-mutating call
  /// (uploadChunkMesh / uploadPbrChunkMesh / clearChunkMeshes). The store
  /// is render-side-owned; since Stage 2 all mutations on the split path
  /// arrive via the upload ring and apply at the seal (the executor's
  /// drain, before the draw-order build reads the store), and a finished
  /// frame's slot records the generation its drain produced.
  uint64_t meshStoreGeneration() const { return storeGen_; }
  void renderFrame(const FrameParams& p) override;
  /// Three-way software split (Design B, Stage 1): execute renderFrame(fp)
  /// detached on the band pool's executor worker (see setDetachedPoolMode)
  /// and return immediately; onDone(err) runs on that worker when the frame
  /// is finished (err null on success). The render executes under
  /// threeway::RendererGuard, exactly as the Vulkan RenderWorker's loop
  /// does, so the game's remaining guarded renderer calls (grabs, settings,
  /// entity uploads) stay serialised with it. Stage 2: before the render,
  /// the executor drains the upload ring up to `drainTarget` (the seal —
  /// see drainUploadRing) with NO guard: the game's mesh uploads no longer
  /// take the guard at all on this path. The caller owns everything fp
  /// borrows (the mailbox job's copies) until onDone fires. If the pool's
  /// detached mode is off, the frame runs inline on the caller and onDone
  /// fires before this returns (the Stage 0 shape) — callers stay correct
  /// either way.
  void renderFrameDetached(const FrameParams& fp, uint64_t drainTarget,
                           const std::function<void(std::exception_ptr)>& onDone);
  /// Pool-wide detached mode (Design B, Stage 1): while on, the shared
  /// cSwBandPool spawns one extra worker — the executor that runs
  /// renderFrameDetached bodies (band count unchanged; synchronous run()
  /// keeps caller-runs-band-0 with the remaining workers, and a detached
  /// body calling run() takes the pool's tlInRun_ inline path). Set by the
  /// app when the software threeway split is enabled; off (the default) is
  /// the pre-Stage-1 pool exactly.
  static void setDetachedPoolMode(bool on);
  RenderStats stats() const override { return stats_; }
  void requestGrab() override { grabArmed_ = true; }
  /// The frame snapshot by the most recent armed renderFrame(), as
  /// top-row-first RGB at framebuffer size. Call on the render thread (the
  /// MCP adapter serializes its renderer externally, as before).
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override;

  // --- Direct CPU surface (pre-interface API; semantics unchanged) -----------

  /// Render a mesh to the framebuffer (CPU rasterization). Vertex positions
  /// are sub-chunk-local; `viewProj` carries any world translation.
  void renderMesh(const PackedVertex* verts, size_t vertCount,
                  const float* viewProj);

  /// Get the framebuffer (for display or PNG export).
  /// Cortana 2026-10-15 1156: chunk-pass raster threads, shared by every
  /// cSoftwareRenderer (game view + MCP vision). 0 = default (4). Rounded
  /// down to a power of two and clamped to hardware threads; applied on the
  /// next frame. Engine-level so mods/options can set it.
  static void setRenderThreads(unsigned n);
  /// Cortana 2026-10-25 (pass sw3, Archie): --sw-scale. The 3D world renders at
  /// scale x the target size (0.5 / 0.75; 1.0 = off), is nearest-neighbour
  /// upscaled, then overlays/HUD/console draw at native resolution. Ignored with SSAA.
  static void setRenderScale(float s);
  /// Effective thread count after rounding/clamping.
  static unsigned renderThreads();
  /// Cortana 2026-10-26 (item 5): while held, LL_SW_SHOT does not count frames (the game holds
  /// it until its mesh queue has drained, so pinned shots see the same geometry every run).
  static void setShotHold(bool hold);
  /// True once the LL_SW_SHOT frame has been written.
  static bool shotTaken();
  /// Cortana 2026-10-15 1450: the auto rule, exposed for tests: pow2 <= (logical - 2), clamped 2..16.
  static unsigned autoRenderThreads(unsigned logicalCores);
  // Cortana 2026-10-15 1610 (ENGINE): Debug Overlay support. Meshes that
  // survived culling in the last renderFrame (the draw list size).
  size_t meshesDrawnLastFrame() const { return drawOrder_.size(); }
  /// One-line status strip, drawn straight into a 0xAARRGGBB image (row 0 at
  /// the top) with the baked 8x16 HUD font over a dark bar. The caller saves
  /// the rows it covers (stripRows()) first and puts them back after present,
  /// so captures never see the strip.
  static void drawStatusStrip(uint32_t* px, int w, int h, const char* text);
  static int stripRows() { return 18; }
  const SoftwareFramebuffer& framebuffer() const { return fb_; }
  SoftwareFramebuffer& framebuffer() { return fb_; }

  void resize(uint32_t w, uint32_t h);
  void clear(uint32_t color = 0xFF000000);
  /// Feeds the texture array directly: `layers` 16x16 uint32 tiles
  /// (0xAARRGGBB). Same storage textureArray().upload() fills — one copy.
  void setTextures(const uint32_t* data, uint32_t layers);
  /// FXAA-lite: 3x3 blend gated on neighbourhood luminance contrast
  /// (flat regions untouched). Not a full FXAA shader; no production
  /// caller yet (P3-3, Appendix D L2).
  void applyFXAA();

  /// Enable/disable per-pixel shading (default: flat for speed).
  void setPerPixelShading(bool enable) { perPixel_ = enable; }

  /// Select the fragment shading tier for the per-pixel path (default Basic).
  /// Basic keeps the existing basicFragmentShader path intact; Pbr runs
  /// pbrFragmentShader (graceful per-fragment fallback to basic without material info).
  void setShadingTier(SoftwareShadingTier tier) { shadingTier_ = tier; }
  SoftwareShadingTier shadingTier() const { return shadingTier_; }

  /// PBR context (registry, lights, emissive provider, camera); must outlive the
  /// renderer. Null (default) means the PBR tier falls back to basic shading.
  void setPbrContext(const PbrFragmentContext* ctx) { pbrContext_ = ctx; }

 private:
  // Render pipeline M8 (software uploader): LL_SW_CANON=1 ingests ChunkMesh::canonical
  // (span decode via decodeCanonicalMesh) with draw-side LOD/skirt selection; default off =
  // the legacy PackedVertex stream. canonSrc_ keeps each canonical mesh + its current lod so
  // a camera ring crossing re-decodes at the new lod (store-owned: drain/renderFrame only).
  struct SwCanonSrc { ChunkMesh meta; int lod = 0; };
  std::map<int64_t, SwCanonSrc> canonSrc_;
  int canonCamCx_ = 1 << 30, canonCamCz_ = 1 << 30;
  bool canonInner_ = false;  ///< storing a decoded stream (keeps canonSrc_)
  void reselectCanonLods(const Vec3& cam);
 public:
  static bool canonIngestEnabled();
  static void setCanonIngest(bool on);  ///< overrides LL_SW_CANON (tests)
  /// Test hook: the lod the canonical path currently draws for a stored chunk (-1 = legacy).
  int canonLodOf(int cx, int sy, int cz) const;
  /// Test hook: move the LOD reference point without a frame (renderFrame does this).
  void setCanonCamera(const Vec3& cam) { reselectCanonLods(cam); }
 private:
  SoftwareFramebuffer fb_;
  std::vector<int> meshRowY0_, meshRowY1_;  ///< RND-H3: per drawn mesh screen row span
  std::vector<float> frameScreen_;  ///< Cortana 2026-10-15 1112: chunk-pass verts transformed once per frame
  std::vector<size_t> meshScreenOff_;
  std::vector<PackedVertex> frameVerts_;  ///< Elfie 2026-10-18 phase B: per-frame expansion of the
                                          ///< packed StoredQuad store (drawn meshes only, indexed
                                          ///< parallel to frameScreen_ via meshVertOff_)
  std::vector<size_t> meshVertOff_;
  struct StripWork { int strip, x0, x1; uint32_t cost; };
  std::vector<StripWork> stripWork_; std::vector<uint32_t> stripCost_;  ///< 2026-10-25 step 1: heaviest-first strips
  std::vector<uint32_t> tileCost_; std::vector<int> tileOrder_;  ///< 2026-10-26 item 6 Candidate D (LL_SW_TILE64)
  std::vector<std::vector<uint32_t>> meshBins_, meshBinOff_;  ///< Cortana 2026-10-15 1255: per-mesh strip bins (CSR)
  std::vector<std::vector<uint32_t>> binFill_;  ///< per band-thread fill cursors
  std::vector<uint16_t> depth_;  ///< PT4-A (2026-10-26): depth buffer, uint16-quantized [0,1] z/w domain (swsimd::depthQ); 0xFFFF = far/clear
  // Cortana 2026-10-15 1715: hierarchical-Z [ENGINE]. One entry per 8x8 tile:
  // an UPPER BOUND on depth_ inside the tile (0xFFFF = not yet tightened). Depth
  // only ever decreases between clears, so a stale entry stays a valid bound;
  // writers mark the tile dirty and the next query re-tightens it. Tiles are
  // 8-row aligned inside the 32-row band strips, so each tile is touched by
  // exactly one band thread per frame (no sharing). Any depth_ re-fill that is
  // not clear() disables it until the next clear() (hizFbW_ = 0).
  static constexpr int kHizTile = 8;
  std::vector<uint16_t> hizMax_;
  std::vector<uint8_t> hizDirty_;
  uint32_t hizFbW_ = 0, hizFbH_ = 0, hizTilesX_ = 0;
  void hizReset();                    ///< after clear(): every tile 0xFFFF, clean
  uint16_t hizTileMax(int tx, int ty);   ///< tightened upper bound of the tile's depth
  std::vector<float> screen_;  ///< reusable vertex transform scratch (fix 2026-10-11:
                               ///< was allocated per renderMesh call)
  std::vector<uint32_t> fxaaSrc_;  ///< reusable applyFXAA source copy (fix 2026-10-11:
                                   ///< was a full-framebuffer copy per call)
  // Model dithering state (JD 2026-10-13); see setModelDither.
  bool ditherEnabled_ = false;  // the game-side setting (default on) enables it
  float ditherNear_ = 2.f;  ///< blocks: at/below this view depth geometry is fully faded
  float ditherFar_ = 8.f;   ///< blocks: at/above this view depth geometry is untouched
  // AA ladder state (JD 2026-10-13); see setAaMode.
  SoftwareAaMode aaMode_{};  ///< value-initialized = 0 = Off
  bool bloom_ = false;       ///< Cortana 2026-10-16: render.bloom post pass
  bool shimmerOn_ = true;    ///< Cortana 2026-10-16: render.shimmer
  bool mipOn_ = true;
  bool emissiveLightOn_ = false;      ///< Cortana 2026-10-16: render.emissive_light
  std::vector<float> frameEmit_;     ///< this frame's nearest emitters (6 floats each)        ///< Cortana 2026-10-16: render.mipmaps (distance LOD, BUG-0005)
  float shimmerTime_ = 0.f;  ///< render clock (s) for the shimmer wave
  std::unique_ptr<SoftwareAaScratch> aaScratch_;  ///< lazily built post-pass scratch
  std::vector<uint32_t> layerAvg_;  ///< 2026-10-25: per texture layer average colour (far-LOD flat shading)
  const uint32_t* layerAvgSrc_ = nullptr; uint32_t layerAvgN_ = 0;
  void refreshLayerAvg();
  std::vector<uint32_t> swScaleX_;                ///< pass sw3: upscale column map
  SoftwareFramebuffer aaOut_;                     ///< SSAA resolve target (renderFrame)
  SoftwareAaScratch& ensureAaScratch();
  // ASCII state (JD 2026-10-13); see setAsciiMode / setAsciiValueSpan.
  bool asciiEnabled_ = false;
  int asciiCols_ = 160;
  int asciiSet_ = 0;  ///< AsciiGlyphSet (0 = Ascii, 1 = cSysterian) as int
  int asciiSpan_ = 255;  ///< value span (JD amendment: full 0-255 default)
  std::string asciiText_;  ///< last ASCII stage's raw text grid
  bool perPixel_ = false;  ///< per-pixel shading (slower, better quality)
  SoftwareShadingTier shadingTier_ = SoftwareShadingTier::Basic;  ///< fragment tier (per-pixel path)
  const PbrFragmentContext* pbrContext_ = nullptr;  ///< PBR context (registry/lights/camera); may be null

  cCpuTextureArray textures_;
  std::array<int, 32> tintMask_{}; std::array<int, 2> stripPal_{};  ///< 2026-10-16 kShadeTraceTint
  std::array<int, 4> leafGlow_{};  ///< 2026-10-16 crystal twinkle glow layers
  int twinkleMode_ = 2; uint32_t twinklePhase_ = 0;
  std::array<int, 3> lampPal_{{-1, -1, -1}}, lampMask_{{-1, -1, -1}}, lampRim_{{-1, -1, -1}};  ///< 2026-10-16: colour-lamp bindings  ///< the one texture store (setTextures + ITextureArray)

  /// Elfie 2026-10-18 (Argon counsel pt 1, phase B): packed per-quad store.
  /// The mesher emits every quad from one Look (mesher.cpp emitFace): layer,
  /// value, mode are identical across the quad's 4 corners, and faceAo splits
  /// into per-quad face (bits 0-2) + rot (bits 5-6) and PER-CORNER ao (bits
  /// 3-4, indexed ao[c]). PackedVertex spends 16 B x 4 = 64 B/quad storing the
  /// shared fields 4x (plus the 24-bit slot, which no software consumer reads).
  /// StoredQuad stores the shared block once + 4 compact corners = 38 B/quad.
  /// Phase 1 of renderFrame expands the drawn range back into byte-identical
  /// PackedVertex scratch (frameVerts_), so every downstream consumer —
  /// transform, varyings, shading — runs on exactly the phase-A bytes.
  struct StoredQuad {
    struct Corner {
      uint16_t x = 0, y = 0, z = 0;  // sub-chunk-local, 1/16 blocks
      uint8_t u = 0, v = 0;          // tile UV, 1/16 units
    };
    uint16_t layer = 0;   // full layer index (layer | layerHi << 8)
    uint8_t value = 0;
    uint8_t mode = 0;
    uint8_t faceRot = 0;  // faceAo bits 0-2 (face) | bits 5-6 (rot); bits 3-4, 7 zero
    uint8_t aoPack = 0;   // per-corner AO (faceAo bits 3-4): corner k at bits 2k..2k+1
    Corner corners[4] = {};
  };
  static_assert(sizeof(StoredQuad) == 38, "StoredQuad layout: 6 B shared + 4 x 8 B corners");

  /// Chunk meshes by (cx,sy,cz), keyed exactly like the GPU backends.
  struct StoredMesh {
    int cx = 0, sy = 0, cz = 0;
    std::vector<PackedVertex> verts;  // legacy store: only for meshes whose quads
                                      // fail the shared-attribute invariance check
    std::vector<StoredQuad> quads;    // phase B packed store (empty <=> legacy)
    size_t storedVerts() const { return quads.empty() ? verts.size() : quads.size() * 4; }
    size_t storedQuads() const { return quads.empty() ? verts.size() / 4 : quads.size(); }
    // Wafer ground pulse + trace params baked into the ChunkMesh at mesh
    // time; renderFrame adds both emissives for kShadeGroundPulse quads
    // through the CPU mirrors (ground_pulse.h / ground_traces.h).
    float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
    uint8_t lodScale = 1;   // 2026-10-25: ChunkMesh::lodScale (>= 4: flat shade + height cull)
    bool hasBlend = false;  // Cortana 2026-10-15 1755: has kShadeAlphaBlend quads (second, blended pass)
    // Cortana 2026-10-16 (JD foliage LOD) / Elfie 2026-10-17 (Argon counsel pt 1, phase A):
    // ONE stored set, reordered at upload: untagged quads first (original relative order),
    // then kShadeLodNear quads with tag bits masked — as a set, exactly the old near set.
    // kShadeLodFar quads are never drawn on the software path (both old sets dropped them)
    // and are not stored. Beyond kLodSwitchBlocks the renderer draws only the prefix
    // [0, untaggedCount) quads — exactly the old far set (JD: software may skip far foliage).
    bool hasLod = false;
    size_t untaggedCount = 0;  // quads in the far-mode prefix of `verts`
    /// Cortana 2026-10-16 (render.emissive_light): this mesh's light emitters, world space,
    /// 6 floats each (x, y, z, r, g, b). Built at upload from emissive / powered-Trace quads.
    std::vector<float> emit;
    /// Ginny (Argon) 2026-10-16 Pass 2 step 3: face-direction buckets. quadFace[q] = 0..5
    /// (+X,-X,+Y,-Y,+Z,-Z: the geometric normal of the quad's own winding, axis-aligned
    /// planar quads only) or 6 (never culled). planeLim[b] = the bucket's limiting plane in
    /// mesh-local blocks (min for + buckets, max for - buckets). Built once over the stored
    /// vector (Elfie 2026-10-17): exact for the near set; in far mode the limits span the
    /// untagged prefix plus the (undrawn) near quads, which only widens them — the bucket
    /// test stays conservative and the draw prefix remains the authority on what is drawn.
    std::vector<uint8_t> quadFace;
    float planeLim[6] = {0, 0, 0, 0, 0, 0};
  };
  std::map<int64_t, StoredMesh> meshes_;
  /// RND-H4 scratch: visible meshes sorted front-to-back each frame (reused, no per-frame alloc).
  std::vector<std::pair<float, const StoredMesh*>> drawOrder_;
  /// CREW TILE (shake 2026-10-18): reused scratch buffers (were per-call
  /// allocations of identical shape). Same single-threaded-per-instance
  /// discipline drawOrder_/frameEmit_ already rely on.
  std::vector<std::pair<float, size_t>> emitCandScratch_;  ///< renderFrame emitter candidates
  std::vector<const float*> emitSrcScratch_;               ///< renderFrame emitter sources
  std::vector<PackedVertex> uploadOrderedScratch_;         ///< uploadChunkMesh LOD reorder set
  std::map<int64_t, PbrChunkMesh> pbrMeshes_;  ///< BinaryQuad uploads, same keying
  uint64_t storeGen_ = 0;  ///< mesh-store seal generation: bumped by every store mutation

  // Three-way software split (Design B, Stage 2): the upload ring and its
  // drain state. The ring itself is SPSC (producer = main, consumer = the
  // detached executor). drainBatch_ and the ring* counters are touched only
  // by the consumer side (the detached body, or the caller when the pool's
  // detached mode is off); ringMaterialsEnqueued_ only by the producer.
  cSwUploadRing uploadRing_;
  bool uploadRingEnabled_ = false;
  const cMaterialReg* ringMaterialsEnqueued_ = nullptr;  ///< last table announced via enqueuePbrMaterials (producer)
  std::atomic<const cMaterialReg*> pendingPbrMaterials_{nullptr};  ///< announced table, applied at the next seal
  std::vector<cSwUploadRing::Slot> drainBatch_;  ///< drained entries between the seal and the post-render retire
  uint64_t ringDrained_ = 0, ringApplied_ = 0, ringCoalesced_ = 0, ringClears_ = 0;  ///< consumer-side stats
  /// The frame seal (executor): apply the announced PBR material table,
  /// then drain the ring up to `target` and apply the survivors of
  /// consumer-side coalescing through the unchanged store entry points.
  void drainUploadRing(uint64_t target);
  /// Argon trap 3: release the drained entries' source buffers — after
  /// renderFrame, outside the seal, so no free contends on the handoff.
  void retireDrainedMeshes() { drainBatch_.clear(); }

  // IRenderer PBR/culling state (H2/M1).
  float frustumMarginDeg_ = 5.f;  ///< culling margin, degrees (JD 2026-10-11)
  CullProtoState cullProto_;      ///< culling-cache prototypes; all default off
  PbrTransparencyTier pbrTier_ = PbrTransparencyTier::ScreenDoor;  ///< recorded tier
  AmbientMode ambientMode_ = AmbientMode::Fraction;
  const cMaterialReg* pbrMaterials_ = nullptr;  ///< setPbrMaterials registry
  PbrFragmentContext ownedPbrCtx_;  ///< context fed by setPbrMaterials/setAmbientMode
  /// FrameParams::pbrLights as an IEmissiveProvider for the owned context:
  /// the game already reduces its emissive scan into that array per frame.
  struct FrameLightsProvider : IEmissiveProvider {
    const FrameParams* frame = nullptr;
    void forEachEmissive(const std::function<void(float, float, float, float, float, float,
                                                  float)>& sink) const override {
      if (!frame) return;
      for (uint32_t i = 0; i < frame->pbrLightCount; ++i) {
        const AnalyticLight& l = frame->pbrLights[i];
        sink(l.x, l.y, l.z, l.r, l.g, l.b, l.intensity);
      }
    }
  };
  FrameLightsProvider frameLights_;
  /// The context fragment shading actually reads: an externally supplied
  /// setPbrContext() wins; otherwise the owned context (when a registry
  /// was supplied via setPbrMaterials); otherwise null (basic fallback).
  const PbrFragmentContext* effectivePbrContext() const {
    if (pbrContext_) return pbrContext_;
    return pbrMaterials_ ? &ownedPbrCtx_ : nullptr;
  }

  // Ground pulse/trace state for the mesh currently being drawn by
  // renderFrame (single-threaded rasterizer; set per mesh, read by the
  // per-pixel shader glue below). Zero params = no contribution.
  struct GroundFx {
    float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseTimeSec = 0.f, traceTimeSec = 0.f;
    bool pulseOn = false, traceOn = false;
    bool active() const {
      return (pulseOn && pulseP0[2] > 0.f) || (traceOn && traceP0[2] > 0.f && traceP0[0] > 0.f);
    }
  };
  // RND-H3 banding (2026-10-15): per-mesh draw state is thread_local so the
  // chunk pass can rasterize horizontal screen bands on several threads.
  static thread_local GroundFx groundFx_;
  static thread_local float meshOriginX_, meshOriginY_, meshOriginZ_;  ///< world origin of the mesh being drawn

  // HUD overlay state (H3): built lazily, reused across frames.
  std::vector<HudVertex> hudVerts_;
  HudFontAtlas hudAtlas_;
  bool hudAtlasReady_ = false;
  std::vector<PackedVertex> entityVerts_;  ///< dynamic triangle verts (3 per triangle)
  std::vector<PackedVertex> spriteScratch_;  ///< renderFrame sprite conversion scratch
  // Sprite average-color cache (P3-3, Appendix D M2): the average of a
  // quad's atlas rect, keyed by (atlas version, page, pixel rect) — the
  // old code rebuilt a temp Image from a fresh vector per quad per frame.
  // Cleared wholesale when the atlas version moves.
  struct SpriteAvgKey {
    uint64_t version = 0;
    int page = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator<(const SpriteAvgKey& o) const {
      if (version != o.version) return version < o.version;
      if (page != o.page) return page < o.page;
      if (x0 != o.x0) return x0 < o.x0;
      if (y0 != o.y0) return y0 < o.y0;
      if (x1 != o.x1) return x1 < o.x1;
      return y1 < o.y1;
    }
  };
  std::map<SpriteAvgKey, std::array<float, 4>> spriteAvgCache_;  ///< RGBA average (RGB alpha-weighted)
  uint64_t spriteAvgVersion_ = UINT64_MAX;  ///< atlas version the cache belongs to
  /// One AlphaBlend/Additive sprite quad in screen space, drawn in
  /// producer order with depth TEST but no depth WRITE (D-M2).
  struct BlendSprite {
    float v[4][4];  // screen-space verts (x, y, depth, clipW)
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    bool additive = false;
  };
  std::vector<BlendSprite> spriteBlendScratch_;
  std::vector<PackedVertex> pbrScratch_;     ///< renderPbrMeshes position conversion scratch
  std::vector<uint32_t> pbrScratchMat_;      ///< per-vertex material ID parallel to pbrScratch_
  RenderStats stats_;
  bool grabArmed_ = false;   ///< requestGrab() arms the next renderFrame()
  bool grabReady_ = false;   ///< an armed frame has been snapshotted
  std::vector<uint8_t> grabRgb_;  ///< top-row-first RGB of the armed frame
  int grabW_ = 0, grabH_ = 0;

  /// Rasterize a single triangle (flat shaded, depth tested, backface culled).
  void rasterizeTriangle(const float* v0, const float* v1, const float* v2,
                         uint32_t color);
  /// Rasterize with per-pixel shader (slower, higher quality). `groundFx`
  /// adds the wafer ground pulse + traces emissives for a tagged quad.
  void rasterizeTriangleShaded(const float* v0, const float* v1, const float* v2,
                               const ShaderVaryings* s0, const ShaderVaryings* s1,
                               const ShaderVaryings* s2, FragmentShaderFn shader,
                               bool groundFx = false);
  /// Ginny (Argon) 2026-10-16 Pass 2: 1 or 2 triangles (a quad) with one shared setup and
  /// one row walk; byte-identical to separate rasterizeTriangleShaded calls.
  void rasterizeTrisShaded(int nt, const float* const (*tv)[3], const ShaderVaryings* const (*ts)[3],
                           FragmentShaderFn shader, bool groundFx);
  /// Transform `count` verts to screen space in the screen_ scratch
  /// (x, y, depth, clipW per vertex); returns the scratch pointer.
  const float* transformVerts(const PackedVertex* verts, size_t count, const float* viewProj);
  /// Elfie 2026-10-18 phase B: expand StoredMesh verts [vertBegin, vertBegin+count)
  /// into PackedVertex scratch (byte-identical to the phase-A stored vector:
  /// shared attributes replicated per corner, per-corner AO restored into
  /// faceAo bits 3-4, dropped slot reconstructed as 0 — no software consumer
  /// reads it). Legacy-store meshes are a plain copy of the same range.
  static void expandStored(const StoredMesh& m, size_t vertBegin, size_t count, PackedVertex* dst);
  /// Elfie 2026-10-18 phase B: pack an ordered PackedVertex quad vector into
  /// StoredQuad form. Returns false (out untouched) if any quad's shared
  /// attributes differ across its corners — the caller keeps the legacy store.
  static bool packQuads(const std::vector<PackedVertex>& ordered, std::vector<StoredQuad>& out);
  /// Transform `count` verts into screen_ and draw them as flat-shaded quads
  /// (4 verts each). The flat path shared by renderMesh and the sprite pass.
  void renderFlatQuads(const PackedVertex* verts, size_t count, const float* viewProj);
  /// Draw the stored entity triangle verts through the current shading path.
  void renderEntityVerts(const float* viewProj);
  /// FrameParams sprite quads in the flat average-color form (the same
  /// conversion the game's sprite host feeds the MCP capture path).
  /// Shade handling (P3-3, Appendix D M2): Cutout (and any non-blend mode)
  /// draws opaque with depth write, as before; AlphaBlend and Additive
  /// quads are drawn in producer order (back-to-front) with depth test
  /// but NO depth write, blended into the framebuffer — previously every
  /// mode was flattened to opaque and occluded like world geometry.
  void drawSprites(const FrameParams& p);
  /// Rasterize one blended sprite triangle pair: depth-tested against
  /// depth_, never writes depth; src-over (AlphaBlend) or additive.
  void rasterizeBlendSprite(const BlendSprite& s);
  /// Draw the stored BinaryQuad (PBR) meshes: per-vertex material IDs shade
  /// through pbrFragmentShader via the effective PBR context.
  void renderPbrMeshes(const FrameParams& p);
  /// CPU HUD rasterizer (H3): buildHudVertices triangles alpha-blended in
  /// paint order (u < 0 = solid; otherwise font-atlas coverage, NEAREST like
  /// the GL33 hud program), no depth test — drawn after the world.
  void drawHud(const FrameParams& p);
  /// One HUD triangle: barycentric blend over fb_ with interpolated
  /// uv/color; coverage from the font atlas for glyph triangles.
  void rasterizeHudTriangle(const HudVertex& a, const HudVertex& b, const HudVertex& c);
  void fillHudRectSpan(const HudVertex& c, int x0, int y0, int x1, int y1);  // 2026-10-25 HUD diet
  void drawHudList(const HudVertex* v, size_t n);  // 2026-10-26: one in-order walk, row-clipped per chunk
  /// Block highlight outline (depth-tested) + crosshair (on top), drawn as
  /// CPU line segments through the same projections the GPU backends use.
  void drawOverlayLines(const FrameParams& p);
  /// Blend one pixel (src-over, straight alpha) at (x, y) if inside fb_.
  void blendPixel(int x, int y, float r, float g, float b, float a);
  /// Plot a depth-tested world-space line segment (highlight outline).
  void drawWorldLine(const FrameParams& p, const Vec3& a, const Vec3& b, uint32_t color);
  /// Plot an NDC line segment ignoring depth (crosshair).
  void drawNdcLine(const Vec3& a, const Vec3& b, uint32_t color);
};

}  // namespace litholattice
