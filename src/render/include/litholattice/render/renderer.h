// Cortana 2026-10-15 1020: GAME-H1 setEntityOrigin
// Renderer interface. Backends (gl33 here, Vulkan in render_vk) implement IRenderer and
// ITextureArray (texture_array.h).
// Render code knows chunk meshes and frame parameters only: nothing about the sim.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/core/math.h"
#include "litholattice/render/cull_prototypes.h"

namespace litholattice {
// Core mesh/shading types, declared early: the PBR headers below pull in
// transparency.h, which needs the complete ShadeMode/PackedVertex definitions.
// (Moved up from below; declaration order only, no semantics change.)
// 16-byte vertex: position in 1/16 block units local to the sub-chunk (sub-block shapes),
// texture UV in 1/16 tile units (already rotated), face | ao << 3 | rot << 5 (rot = quarter turns
// applied to the UV, so shaders can recover the unrotated tile UV for masks), texture-array layer,
// shading value (meaning set by the mode: tint strength, palette index, pip / cell count) and shading mode,
// then the 32-bit buffer slot (mesher-side use; not consumed by the classic chunk shaders).
// LAYOUT CONTRACT (fix 2026-10-11): the first 12 bytes (x,y,z,u,v,faceAo,layer,value,mode) are the
// stable GPU vertex-attribute prefix — gl33/vk binding sites read @0/@6/@10. `slot` rides at @12.
// PBR VERTEX-FORMAT NOTE (phase 2, Worker 3 - JD's locked coexistence decision,
// 2026-10-11, SUPERSEDES the phase-1 "Option B" note below): BinaryQuad and
// PackedVertex COEXIST as a user-selectable mesher option; PackedVertex is NEVER
// extended and no parallel attribute stream was added. The BinaryQuad path carries
// material IDs natively (BinaryQuad::material = 8-bit palette index, passthrough)
// through its own PbrQuadVertex layout (render/pbr_quad_mesh.h) into the chunk_pbr
// programs; the PackedVertex path keeps the classic chunk programs, untouched -
// classic-mesh fragments still resolve to the 0xFFFF "none" marker and the PBR tier
// falls back to basic shading per fragment (pinned by the engine tests' pbr-seam
// checks). Phase 1's lean-Option-B deliberation is kept below for the record only.
//   (phase-1 text: A. Extend PackedVertex (e.g. +uint16 material -> 18 bytes)...
//   B. Parallel attribute stream... Lean was B first. Final call was JD's: NEITHER -
//   coexistence via the BinaryQuad path instead.)
#pragma pack(push, 1)
// Cortana 2026-10-15 1628: CA-03 [ENGINE] 16-bit texture layer. The high layer byte lives in the
// top 8 bits of the old 32-bit slot word, so the struct stays 16 bytes (the JD-locked
// "never extended" rule holds). slot keeps 24 bits; no consumer reads it from vertices.
struct PackedVertex {
  uint16_t x, y, z; uint8_t u, v, faceAo, layer, value, mode; uint32_t slot : 24; uint32_t layerHi : 8;
  uint32_t layerIndex() const { return uint32_t(layer) | (uint32_t(layerHi) << 8); }
  void setLayer(uint32_t l) { layer = uint8_t(l & 0xFFu); layerHi = uint8_t((l >> 8) & 0xFFu); }
};
#pragma pack(pop)
static_assert(sizeof(PackedVertex) == 16, "vertex layout");

enum ShadeMode : uint8_t {
  kShadeTex = 0,      // plain texture
  kShadeEmissive,     // texture, self-lit
  kShadeCutout,       // texture with alpha test (sprites)
  kShadeStripTint0, kShadeStripTint1,                 // neutral strip piece tinted by palette row [value] + ring mask (bindings strip 0 / 1)
  kShadePalette16, kShadePalette32, kShadePalette64,  // palette colour [value] (wrap) inside the tier mask + rim
  kShadePips,         // value 1..8 pips via mask
  kShadeCells,        // value 1..64 cells via mask
  kShadeAnimated,     // animated face: value = animation index (frame strip + timing)
  kShadeGlass,        // translucent (screen-door: alpha vs a 4x4 ordered dither; no sorting needed)
  kShadeAlphaBlend,   // alpha blending (src_alpha, 1-src_alpha); needs back-to-front sorting
  kShadeWeightedOIT,  // weighted blended order-independent transparency; no sorting
  kShadeAdditive,     // additive blending (src_alpha, 1); no sorting; for particles/glow
  kShadeReadout,      // 7-segment readout overlay: layer = segment mask (R = id x16, G = coverage), value = number | 128 lit
  kShadeGroundPulse,  // wafer ground pulse (JD 2026-10-11): textured face + time-driven
                      // emissive add; value = GroundPulseBiome id (1..5, ground_pulse.h).
                      // Appended last: existing modes keep their values.
  kShadeLeaf,         // 2026-10-16 (JD leaf pass): fancy leaf, alpha test 0.5 + crystal twinkle;
                      // value = 1-based leaf glow slot (MaterialBindings::leafGlow), 0 = no glow
  kShadeTraceTint,    // 2026-10-16 (Wafer beta art): base texel + grey tint mask (R) x Trace colour;
                      // value bits 0-4 = tint slot 0..31 (MaterialBindings::kTintSlots = 32; shader reads vVal & 31; bit 5 unused; Cortana 2026-10-16 2335 F55-3), bit 6 = blue Trace,
                      // bit 7 = powered (full ramp + emissive). Off = the ramp's low end.
};
// Cortana 2026-10-16 [ENGINE] (JD foliage LOD): the top two bits of PackedVertex::mode tag LOD
// geometry; the low 6 bits are the ShadeMode. Untagged quads always draw. A sub-chunk whose
// centre is more than kLodSwitchBlocks (horizontal) from the camera draws its kShadeLodFar quads
// instead of its kShadeLodNear ones. GPU backends decide per draw in chunk.vert; the software
// backend keeps two pre-split vertex sets per mesh (selectLod). Render-only.
inline constexpr uint8_t kShadeModeMask = 0x3F, kShadeLodNear = 0x40, kShadeLodFar = 0x80;
inline constexpr float kLodSwitchBlocks = 32.f;
inline bool hasLodTags(const std::vector<PackedVertex>& v) {
  for (size_t q = 0; q < v.size(); q += 4) if (v[q].mode & (kShadeLodNear | kShadeLodFar)) return true;
  return false;
}
/// Quads of `in` drawn at the given LOD (untagged + the matching tag), mode bits masked.
inline void selectLod(const std::vector<PackedVertex>& in, bool far, std::vector<PackedVertex>& out) {
  out.clear();
  out.reserve(in.size());
  const uint8_t drop = far ? kShadeLodNear : kShadeLodFar;
  for (size_t q = 0; q + 3 < in.size(); q += 4) {
    if (in[q].mode & drop) continue;
    for (size_t k = 0; k < 4; ++k) { PackedVertex v = in[q + k]; v.mode = uint8_t(v.mode & kShadeModeMask); out.push_back(v); }
  }
}
}  // namespace litholattice

#include "litholattice/render/pbr_lights.h"  // AnalyticLight for FrameParams::pbrLights
#include "litholattice/render/wafer_reveal.h"  // Thread Handshake P7
#include "litholattice/render/pbr_transparency_tiers.h"  // PbrTransparencyTier for setPbrTier

namespace litholattice {

class BlockMaterials;
class ITextureArray;
class cMaterialReg;  // pbr_materials.h (forward: only used as a const ref here)
class ShaderLibrary;
struct PbrChunkMesh;  // pbr_quad_mesh.h (forward: only used as a const ref here)


/// Most quads one sub-chunk mesh may hold. The mesher enforces it (and logs if it ever trims);
/// both backends size their shared quad index buffer from it, so they can trust every mesh.
constexpr uint32_t kMaxQuadsPerMesh = 16384;

struct CanonicalMesh;  // unified_mesher.h (render pipeline M2)
struct PreparedChunkMeshHandle;  // below IRenderer (Cortana 2026-10-25 upload pass)

struct ChunkMesh {
  int cx = 0, sy = 0, cz = 0;
  uint32_t epoch = 0;
  uint64_t seq = 0;
  // Cortana 2026-10-15 1350 - handshake-coupled meshing, phase-boundary
  // publish (idea: JD; refinement: Cortana): every sub-chunk snapshotted by
  // one cSimThread::flushDirty() (one settled post-phase world state) shares
  // a batch id; batchSize = how many. 0 = not part of a publish batch.
  uint64_t batch = 0;
  uint32_t batchSize = 0;
  std::vector<PackedVertex> verts;  // quads (4 verts each); empty = remove the mesh
  /// Wafer ground pulse (JD 2026-10-11): this chunk column's weight-blended
  /// pulse params, baked at mesh time (biome weights are worldgen-static, so no
  /// per-frame work). p0 = (freqHz, sharpness, amplitude, 0),
  /// p1 = (tintR, tintG, tintB, 0). The backends push them per draw call for
  /// kShadeGroundPulse quads; all-zero = no pulse (e.g. no provider wired).
  float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
  float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
  /// Wafer ground traces (JD 2026-10-11): this chunk column's weight-blended
  /// trace params, baked at mesh time (same biome weight vector as the pulse).
  /// t0 = (density, speed, amplitude, dirMix), t1 = (tintR, tintG, tintB, 0).
  /// The backends push them per draw call for kShadeGroundPulse quads;
  /// all-zero = no traces. Composes additively with the pulse.
  float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
  float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
  /// Cortana 2026-10-25 (box, upload pass): set by a mesh worker when the backend packed this mesh
  /// into GPU memory off the main thread (LL_ASYNC_CHUNK_UPLOAD). Null = upload on the main thread.
  /// Dropping the last owner without publishing hands the slice back (see the handle's destructor).
  std::shared_ptr<PreparedChunkMeshHandle> prepared;
  /// 2026-10-25 (box): far-LOD scale this mesh was built at (1, 2 or 4); the software renderer
  /// flat-shades / height-culls >= 4x meshes. Other backends ignore it.
  uint8_t lodScale = 1;
  /// Render pipeline M2 (2026-10-29): the canonical packed mesh (unified-mesher mode only;
  /// null otherwise). Today it holds the L0 merged faces; uploaders read it from step 4 on.
  std::shared_ptr<const CanonicalMesh> canonical;
};

// 2D overlay in pixels (origin top-left). Generic: the renderer knows nothing about the game.
constexpr int kHudGlyphW = 8, kHudGlyphH = 16;  // baked font cell, multiplied by HudLayer::scale
struct HudRect { float x, y, w, h; float r, g, b, a; };
struct HudText { float x, y; std::string text; float r = 1, g = 1, b = 1, a = 1; };
/// Clip rectangle in pixels; the default clips nothing.
struct HudClip { float x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f; };
/// One retained-UI draw, painted in order after the rects / texts above (so overlapping
/// windows layer correctly) and clipped on the CPU. Empty text = solid rect.
struct HudDraw {
  float x = 0, y = 0, w = 0, h = 0;  ///< rect (w, h unused for text)
  float r = 1, g = 1, b = 1, a = 1;
  float scale = 1.f;                 ///< text: glyph cell multiplier
  std::string text;
  HudClip clip;
};
struct HudLayer {
  float scale = 1.f;
  std::vector<HudRect> rects;
  std::vector<HudText> texts;
  std::vector<HudDraw> draws;  ///< UI (ui::IUiRenderer through cHudUiRenderer)
};

/// One world-space textured quad for the dynamic sprite pass (sprite system
/// integration, 2026-10-12). Corners are world blocks, CCW seen from the
/// front ([0] bottom-left, [1] bottom-right, [2] top-right, [3] top-left);
/// uvs are atlas image space (u right, v down from the page's top-left — a
/// backend uploading page images row-first pairs them with the corners
/// exactly as given). `shade` is one of kShadeCutout / kShadeAlphaBlend /
/// kShadeAdditive; `page` indexes FrameParams::spriteAtlasPages. The game's
/// sprite host converts the sprite system's emitted quads into these per
/// frame; backends never see sprite-module types.
struct SpriteDrawQuad {
  Vec3 corners[4]{};
  float uv[4][2]{};
  float r = 1.f, g = 1.f, b = 1.f, a = 1.f;  ///< tint multiplier
  ShadeMode shade = kShadeCutout;
  int32_t page = -1;
  float sortDepth = 0.f;  ///< view depth (larger = farther); producer-sorted
};

struct FrameParams {
  int width = 0, height = 0;
  Mat4 viewProj;
  Vec3 camPos, sky;
  float yawRad = 0.f, pitchRad = 0.f;  ///< camera orientation, radians (cPlayer::forward convention);
                                       ///< feeds the velocity-adaptive culling prototype (all off by default)
  float fogEnd = 200.f;
  bool highlight = false; int hx = 0, hy = 0, hz = 0;  // outlined block
  bool crosshair = true;
  float timeSec = 0;     // animation clock
  /// 2026-10-16 crystal twinkle (render.twinkle): 0 off, 1 static, 2 on. Driven by timeSec
  /// (render clock) only; never read by the sim.
  int leafTwinkle = 2;
  /// JD Vulkan push (Cortana 2026-10-16): Vulkan-only effects, each a Graphics toggle. GL and the
  /// software renderer ignore these and keep their current look. All render-only.
  static constexpr int kVkFxNormalMaps = 1, kVkFxLighting = 2, kVkFxBloom = 4, kVkFxPulse = 8,
                       kVkFxParticles = 16, kVkFxFog = 32, kVkFxBulbBlend = 64;
  int vkFx = 127;              ///< kVkFx* bits
  float vkBloomStrength = 1.f; ///< bloom gain
  float vkHeartbeatGlow = 1.75f; ///< 0.0.1.53 [C53-3] (Proposed by JD, Built by Cortana): wafer heartbeat -> bloom gain (was the shared 1.35); bloom bit only
  float vkFogDensity = 0.0045f;
  int vkParticleCount = 3072;
  /// Wafer ground pulse (JD 2026-10-11): sim-time seconds driving the surface
  /// pulse (worldTick / tick rate - pausing the sim freezes it). groundPulseOn
  /// is the live `render.ground_pulse` cvar (default on); off costs one uniform
  /// branch per fragment, no remesh.
  float groundPulseTimeSec = 0;
  bool groundPulseOn = true;
  /// Wafer ground traces (JD 2026-10-11): sim-time seconds driving the trace
  /// packet flow (same clock as the pulse - pausing the sim freezes it).
  /// groundTraceOn is the live `render.ground_traces` cvar (default on),
  /// independent of `render.ground_pulse`; both add into the fragment.
  float groundTraceTimeSec = 0;
  bool groundTraceOn = true;
  bool debugOverlay = false;  // F3: debug-only shading (wire strength numbers)
  HudLayer hud;
  /// PBR analytic lights for the BinaryQuad path (phase 2, Worker 3): the game fills
  /// these per frame from a LightCollector fed by cChunkEmissiveProvider
  /// (pbr_emissive_provider.h); the GL backend uploads them to the chunk_pbr
  /// uPbrLightPos/uPbrLightColor uniforms. Empty = specular from analytic lights off
  /// (flood-fill irradiance still shades). Backends without a PBR path ignore it.
  static constexpr size_t kMaxPbrFrameLights = 8;  // mirrors PBR_MAX_LIGHTS in the shaders
  AnalyticLight pbrLights[kMaxPbrFrameLights];
  uint32_t pbrLightCount = 0;
  /// Sprite pass (sprite system integration, 2026-10-12): this frame's sprite
  /// quads, pre-ordered by the producer (cutout first in spawn order, then
  /// alpha-blend / additive back-to-front), and the atlas pages they sample.
  /// A backend with a sprite path (GL33) re-uploads the pages when
  /// spriteAtlasVersion differs from the version it last uploaded and draws
  /// the quads after the world, before lines/HUD. Backends without a sprite
  /// path ignore both fields (same standing as the PBR fields above). All
  /// pointed-to / spanned data is borrowed: valid for this renderFrame call.
  const SpriteDrawQuad* spriteQuads = nullptr;
  size_t spriteQuadCount = 0;
  std::span<const Image> spriteAtlasPages{};
  uint64_t spriteAtlasVersion = 0;
};

struct RenderStats { uint32_t meshes = 0, drawn = 0; };

/// 0.0.1.53 [C53-2] (Proposed by JD, Built by Cortana): a chunk mesh a worker thread already wrote into GPU-visible
/// memory (IRenderer::prepareChunkMeshAsync). The main thread only publishes it
/// (publishChunkMesh) or hands it back (discardChunkMesh). block/offset/size are
/// renderer-private handle fields.
struct PreparedChunkMesh {
  int cx = 0, sy = 0, cz = 0;
  bool ready = false;
  bool blend = false;
  uint32_t indexCount = 0;
  uint32_t block = 0;
  uint64_t offset = 0, size = 0;
  float pulseP0[4]{}, pulseP1[4]{}, traceP0[4]{}, traceP1[4]{};
  uint32_t pullFirst = 0;   ///< Mega-Pipeline Phase 2 (Vulkan): first packed quad (8-byte index in the block)
  uint32_t pullBucket[7]{};  ///< Mega-Pipeline Phase 2/3 (Vulkan): packed quads by face, [b[f], b[f+1]); b[6] = count
  uint64_t epoch = 0;  ///< F56-2 (Cortana 2026-10-17): renderer mesh epoch at prepare time; stale -> freed on publish
  /// Review P1 (fix pass 2026-10-25): bytes the worker packed for a device-only slice (empty when it wrote the
  /// slice in place). publishChunkMesh stages them, so the main thread never re-encodes the mesh.
  std::vector<uint8_t> staged;
};

class IRenderer {
 public:
  virtual ~IRenderer() = default;
  virtual const char* name() const = 0;
  virtual std::string deviceInfo() const = 0;  // e.g. GL vendor | renderer | version
  /// How frames are presented (Vulkan: fifo / mailbox / immediate; GL: the window's swap interval).
  virtual std::string presentMode() const { return "swap-interval"; }
  /// The block texture array (layer = TextureId). Upload a cTextureReg to it on (re)load.
  virtual ITextureArray& textureArray() = 0;
  /// Material uniforms (mask layers, palette rows, animations) from the block models.
  virtual void setMaterials(const BlockMaterials& materials) = 0;
  /// (Re)builds the shader programs from `shaders` (pack files). A program that is missing or
  /// fails to compile falls back to the built-in base copy, with a log line. Returns the number
  /// of programs that use pack sources.
  virtual int loadShaders(const ShaderLibrary& shaders) = 0;
  virtual void uploadChunkMesh(const ChunkMesh& mesh) = 0;  // replace by (cx,sy,cz)
  /// Three-way software split (Design B, Stage 2): staged-upload entry
  /// points. The defaults apply immediately — exactly the direct calls they
  /// replace — so every backend that does not stage uploads behaves as
  /// before. A backend with an upload ring (the software renderer while the
  /// split is enabled) overrides them to stage the mesh in its lock-free
  /// SPSC ring instead; the render side applies staged entries, in order,
  /// at the frame seal through the unchanged uploadChunkMesh /
  /// uploadPbrChunkMesh / clearChunkMeshes above. Returns false only when a
  /// ring is present AND full: the mesh is then UNTOUCHED (ownership stays
  /// with the caller) and the caller retries later — staged uploads are
  /// deferred, never dropped, and the producer never blocks.
  virtual bool enqueueChunkMesh(ChunkMesh&& mesh) { uploadChunkMesh(mesh); return true; }
  virtual bool enqueuePbrChunkMesh(PbrChunkMesh&& mesh) { uploadPbrChunkMesh(mesh); return true; }
  virtual bool enqueueClearChunkMeshes() { clearChunkMeshes(); return true; }
  /// Staged form of setPbrMaterials (defined with the PBR surface below):
  /// the default applies immediately; a ring backend announces the table
  /// and applies it at the next seal, ahead of that seal's meshes (the
  /// game's update order: materials first, then uploads).
  virtual void enqueuePbrMaterials(const cMaterialReg& reg) { setPbrMaterials(reg); }
  /// True when this backend stages mesh uploads for the render side to
  /// drain (Stage 2 ring): the game's upload phase then takes no
  /// RendererGuard — nothing waits in line behind the in-flight render.
  virtual bool meshUploadsQueued() const { return false; }
  /// 2026-10-25 stutter pass: mesh pool fill in [0, 1] (live mesh bytes / device-local budget), or -1
  /// when the backend has no bounded pool. The game stops admitting far columns above 0.9.
  virtual float meshPoolFill() const { return -1.f; }
  /// 0.0.1.53 [C53-2] (Proposed by JD, Built by Cortana): ANY thread. Writes a non-empty mesh straight into
  /// mapped GPU memory (Vulkan ReBAR / host arena) and fills `out`. false = not supported
  /// here (no mapped memory, opted out, out of memory): use uploadChunkMesh on the main thread.
  virtual bool prepareChunkMeshAsync(const ChunkMesh& mesh, PreparedChunkMesh& out) { (void)mesh; (void)out; return false; }
  /// Main thread: make a prepared mesh live (replaces (cx,sy,cz) like uploadChunkMesh).
  virtual void publishChunkMesh(const PreparedChunkMesh& p) { (void)p; }
  /// Main thread: give back a prepared mesh that will not be published (column dropped).
  virtual void discardChunkMesh(const PreparedChunkMesh& p) { (void)p; }
  virtual void uploadEntityMesh(const std::vector<PackedVertex>& verts) = 0;  // dynamic (items, etc.)
  /// GAME-H1 (2026-10-15): entity verts use the chunk encoding (1/16 block,
  /// relative to an origin); this is that batch's world origin. Default: no-op
  /// for backends that do not draw entities yet.
  virtual void setEntityOrigin(float x, float y, float z) { (void)x; (void)y; (void)z; }
  virtual void clearChunkMeshes() = 0;
  /// F56-1 (Cortana 2026-10-17): the GPU device was lost (Vulkan); the game should close cleanly.
  virtual bool deviceLost() const { return false; }
  /// F56-4: last GPU frame time in ms (timestamp queries), or < 0 when the backend has none.
  virtual float gpuFrameMs() const { return -1.f; }
  /// PBR (BinaryQuad) path (phase 2, Worker 3). Defaults are no-ops / unsupported so the
  /// Vulkan backend, the null/headless renderers and older game builds keep compiling;
  /// the GL33 backend overrides all four. Program selection follows the meshes, not a
  /// mode flag: classic chunk programs draw PackedVertex uploads, chunk_pbr draws
  /// PbrChunkMesh uploads - both maps can coexist in one frame.
  virtual bool supportsPbrPath() const { return false; }  ///< chunk_pbr built and usable
  /// Thread Handshake P7: the wafer reveal (per-column spawn timestamps). Backends without it
  /// (GL / software) ignore the call and say so here, so the game can skip the reveal.
  virtual bool supportsWaferReveal() const { return false; }
  virtual void setWaferReveal(const WaferRevealFrame&) {}
  // Cortana 2026-10-15 1705: greedy meshing [ENGINE]
  /// True when the backend tiles chunk UVs beyond 1 per block (greedy-merged quads).
  /// GPU backends clamp (CLAMP_TO_EDGE) and keep per-block quads. Constant; any thread.
  virtual bool prefersGreedyMeshes() const { return false; }
  /// Uploads the cMaterialReg as the GPU material table (48-byte std430 rows,
  /// pbr_gl_upload.h). Backends re-upload only when reg.generation() moved.
  virtual void setPbrMaterials(const cMaterialReg& reg) { (void)reg; }
  /// Selects the PBR transparency tier. BACKEND AVAILABILITY (exact, P3-3
  /// Appendix D M7): the tier ladder is implemented ONLY on the GL33
  /// backend for BinaryQuad (PbrChunkMesh) uploads — Simple / ScreenDoor /
  /// PbrReflections via the tier uniform, TrueRefraction as the two-pass
  /// scene-color path. The software backend records the tier and runs its
  /// PBR fragment path but implements NO tier ladder (fragments shade
  /// opaque; logged once in setPbrTier). Vulkan has no PBR chunk path at
  /// all. The CPU helper shadeTransparent() implements Simple /
  /// ScreenDoor / PbrReflections per fragment; its TrueRefraction branch
  /// is a request stub (refractionRequested + black). Default ScreenDoor.
  virtual void setPbrTier(PbrTransparencyTier t) { (void)t; }
  /// Selects the flood-fill ambient computation (Fraction = cheap default,
  /// NormalizedSun = direction-aware hemisphere). Default Fraction.
  /// Backends apply it to the PBR diffuse path (CPU + GPU); the classic path is
  /// unaffected. JD-locked 2026-10-11 (cvar `render.ambient_mode`).
  virtual void setAmbientMode(AmbientMode m) { (void)m; }
  /// Angular frustum-culling margin in degrees (0..15, default 5): chunks inside
  /// FOV +/- margin render; the rest cull. The shock absorber against "frustum
  /// occlusion tearing" (JD 2026-10-11): fast rotation reveals already-rendered
  /// chunks instead of pop-in. JD-locked 2026-10-11 (cvar `render.frustum_margin_deg`).
  virtual void setFrustumMarginDeg(float degrees) { (void)degrees; }
  /// Near-camera model dithering (JD 2026-10-13): geometry whose view depth
  /// falls inside [nearBlocks, farBlocks] fades out through a deterministic
  /// screen-door discard (interleaved gradient noise on the pixel position;
  /// render/model_dither.h) in the shading stage, so close-up cameras —
  /// free cams, MCP/agent cameras, Daemon-vision feeds — are not swallowed
  /// by geometry. Purely presentational: no sim interaction, no remesh.
  /// Default here is an honest no-op (unsupported): the software backend
  /// and GL33 implement it; Vulkan does not yet (its chunk program shares
  /// chunk_shading.glsl, but the band uniforms have no push-constant /
  /// descriptor plumbing on that backend — see Model-Dithering-2026-10-13.md).
  /// Driven by the render.model_dither{,_near,_far} cvars.
  virtual void setModelDither(bool enabled, float nearBlocks = 2.f, float farBlocks = 8.f) {
    (void)enabled;
    (void)nearBlocks;
    (void)farBlocks;
  }
  /// Software-renderer anti-aliasing ladder (JD 2026-10-13): named modes
  /// "off" | "fxaa" | "cmaa" | "ssaa2x" behind the render.aa cvar
  /// (render/software_aa.h). CPU-framebuffer techniques, so only the
  /// software backend implements this; the GPU backends keep their own
  /// MSAA story and inherit this honest no-op.
  virtual void setAaMode(const char* modeName) { (void)modeName; }
  /// Cortana 2026-10-16 (render.bloom): software-backend bloom post pass; GPU backends no-op.
  virtual void setBloom(bool on) { (void)on; }
  /// Cortana 2026-10-16: software-backend render.shimmer / render.ao / render.tex_res; GPU no-op.
  virtual void setShimmer(bool on) { (void)on; }
  virtual void setMipmaps(bool on) { (void)on; }
  /// PROVISIONAL (JD 2026-10-24): texture-filtering stand-in for the block texture array.
  /// filter: 0 = Sharp (nearest mag, trilinear-mipmapped min; the old default), 1 = Bilinear,
  /// 2 = Trilinear. aniso: 1 = off, else 2..16, clamped to maxAnisotropy(). GPU backends only;
  /// the software rasterizer has no anisotropic sampling, so it keeps this no-op.
  virtual void setTextureFiltering(int filter, int aniso) { (void)filter; (void)aniso; }
  /// Highest anisotropy the backend can use (1 = unsupported).
  virtual int maxAnisotropy() const { return 1; }
  virtual void setEmissiveLight(bool on) { (void)on; }
  virtual void setAo(bool on) { (void)on; }
  virtual void setTexRes(int tile) { (void)tile; }
  /// ASCII output pipeline (experiment tier, JD 2026-10-13): when on, the
  /// software backend converts its finished frame into a glyph grid
  /// (render/software_ascii.h) — the Daemon Sight low-end configuration.
  /// Driven by render.ascii / render.ascii_cols. No-op elsewhere.
  virtual void setAsciiMode(bool enabled, int cols) {
    (void)enabled;
    (void)cols;
  }
  /// Glyph set for the ASCII pipeline (JD 2026-10-13): "ascii" or
  /// "systerian" (render.ascii_set) — the cSysterian set renders the
  /// engine's certificate-signing script through its ASCII layer.
  virtual void setAsciiGlyphSet(const char* setName) { (void)setName; }
  /// Value span for the ASCII/cSysterian stage (JD-RULED AMENDMENT
  /// 2026-10-13): the luminance -> level mapping spans the FULL 0-255
  /// range by default (render.ascii_value_span = 255) — a deliberate
  /// exception to the game's 64-scale conventions. No-op elsewhere.
  virtual void setAsciiValueSpan(int span) { (void)span; }
  /// Culling-cache prototype toggles (JD 2026-10-11; all default OFF): comet
  /// tail, velocity-adaptive margin, two-tier margin. See cull_prototypes.h.
  virtual void setCullProtoConfig(const CullProtoConfig& cfg) { (void)cfg; }
  /// Per-frame prototype stats (full / meshOnly / culled / cometKept).
  virtual CullProtoStats cullProtoStats() const { return {}; }
  /// Replace the PBR mesh by (cx,sy,cz); empty verts = remove.
  virtual void uploadPbrChunkMesh(const PbrChunkMesh& mesh) { (void)mesh; }
  /// Draws one frame. OpenGL: the caller presents (Window::swap). Vulkan presents itself.
  virtual void renderFrame(const FrameParams& p) = 0;
  virtual RenderStats stats() const = 0;
  /// Arms a one-shot framebuffer grab: the backend makes the frame drawn by
  /// the next renderFrame() available to grabFramebuffer(). Default: no-op
  /// (grabFramebuffer() then reports false). The screenshot command arms the
  /// grab when it runs (during update, before the frame is drawn).
  virtual void requestGrab() {}
  /// Reads back the most recently rendered frame as top-row-first RGB
  /// (resized to w*h*3). Returns false when unsupported, or when no armed
  /// frame has been drawn yet. OpenGL: glReadPixels of the back buffer —
  /// call after renderFrame(), before present. Vulkan: an image-to-host
  /// copy recorded while the armed frame was drawn (before present). Call on
  /// the render thread.
  virtual bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
    (void)rgb;
    (void)w;
    (void)h;
    return false;
  }
};

using GLProcLoader = void* (*)(const char* name);
// OpenGL 3.3 core backend. Needs a current GL context; starts with the built-in shaders and an
// empty texture array. Returns nullptr on failure.
/// `arena`: chunk meshes share one growable VBO + one VAO (glBufferSubData, base-vertex
/// draws); false = one VBO + VAO per sub-chunk (the old path, kept for A/B benches).
std::unique_ptr<IRenderer> createGL33Renderer(GLProcLoader loader, bool arena = true);

/// Cortana 2026-10-25 (box, upload pass): owns a worker-prepared mesh until the main thread publishes it.
/// Destroyed unpublished (coalesced by a newer mesh, stale epoch/seq, dimension switch) -> discarded,
/// so no arena slice leaks. Created on a mesh worker; destroyed/published on the main thread.
struct PreparedChunkMeshHandle {
  IRenderer* owner = nullptr;
  PreparedChunkMesh mesh;
  PreparedChunkMeshHandle() = default;
  PreparedChunkMeshHandle(const PreparedChunkMeshHandle&) = delete;
  PreparedChunkMeshHandle& operator=(const PreparedChunkMeshHandle&) = delete;
  ~PreparedChunkMeshHandle() { if (owner && mesh.ready) owner->discardChunkMesh(mesh); }
  /// Main thread: make it live; afterwards the destructor does nothing.
  void publish() { if (owner && mesh.ready) { owner->publishChunkMesh(mesh); mesh.ready = false; } }
};

}  // namespace litholattice
