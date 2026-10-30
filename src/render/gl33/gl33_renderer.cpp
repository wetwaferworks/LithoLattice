// Cortana 2026-10-15 1020: GAME-H1 entity origin
// OpenGL 3.3 core backend for IRenderer. Owns all GL objects; call only from the thread
// that owns the GL context (the main/render thread).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <cstdlib>
#include <unordered_map>
#include <string_view>
#include "builtin_shaders.h"
#if defined(__wasm__)
// Phase 2 mega-draw verbs (thin layer, lk_webio.js / lkboot.js):
// WEBGL_multi_draw entry points are extension functions and absent from the
// GL proc-address table, so the probe and the draw travel as reserved
// stream ops instead of loader-resolved GL calls.
extern "C" __attribute__((import_module("env"), import_name("lk_mega_probe"))) int lk_mega_probe();
extern "C" __attribute__((import_module("env"), import_name("lk_mega_draw"))) int lk_mega_draw(int mode, const void* counts, const void* offsets, int drawCount);
#endif
#include "gl_loader.h"
#include "gl_program.h"
#include "gl_texture_array.h"
#include "GlTboSlabUploader.h"  // refactor step 3: IMeshUploader over the fix (c) slab pool
#include "litholattice/assets/shader_library.h"
#include "litholattice/core/log.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/pbr_gl_upload.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/mesher.h"          // M12: buildCanonPullStream
#include "litholattice/render/unified_mesher.h"  // M12: CanonicalMesh
#ifndef GL_RGBA32UI
#define GL_RGBA32UI 0x8D70
#endif

namespace litholattice {
namespace {

using gl33::cGlProgram;
using gl33::cGlTextureArray;

/// Programs the backend draws with; names match assets/shaders/<name>.vert/.frag.
/// "chunk_pbr" is the PBR path (BinaryQuad meshes); the classic "chunk" program keeps
/// serving PackedVertex meshes - selection follows the uploaded meshes, not a flag.
constexpr std::string_view kProgramNames[] = {"chunk", "hud", "line", "chunk_pbr", "sprite"};
constexpr size_t kChunkPbrProgram = 3;
constexpr size_t kSpriteProgram = 4;  // sprite pass (FrameParams::spriteQuads); non-fatal like chunk_pbr

/// Interleaved sprite-pass vertex (pos / atlas uv / tint), streamed per frame.
struct SpriteVert { float x, y, z, u, v, r, g, b, a; };

/// Built-in copy of a base-pack shader file (generated from assets/shaders at build time).
std::optional<std::string> builtinFile(std::string_view file) {
  for (const BuiltinShaderFile& f : kBuiltinShaderFiles)
    if (file == f.name) return std::string(f.source);
  return std::nullopt;
}

/// Built-in shader stage with its includes expanded from the other built-in files.
std::string builtinSource(std::string_view file, std::string* error) {
  const std::optional<std::string> src = builtinFile(file);
  if (!src) { *error = "missing built-in file " + std::string(file); return {}; }
  return expandShaderIncludes(*src, builtinFile, error).value_or(std::string{});
}

/// Render pipeline M12 (GL TBO uploader, LL_GL_CANON=1): wrap the chunk vertex shader so
/// it can pull its four legacy attributes from the canonical pull stream (RGBA32UI buffer
/// texture, canonical_pull_legacy.glsl) instead of the vertex arrays. uCanonMode 0 keeps
/// the vertex-array path (legacy meshes, entities), so one program serves both and the
/// shading code below the attributes is the unmodified chunk.vert by construction.
std::optional<std::string> canonPullVariant(const std::string& vs, std::string* why) {
  static const std::regex attr(R"(layout\s*\(\s*location\s*=\s*([0-3])\s*\)\s*in\s+(uvec[234]|uint)\s+(aPos|aInfo|aShade|aLayerHi)\s*;)");
  std::string out;
  int found = 0;
  auto it = std::sregex_iterator(vs.begin(), vs.end(), attr);
  size_t last = 0;
  for (; it != std::sregex_iterator(); ++it) {
    const std::smatch& m = *it;
    out += vs.substr(last, size_t(m.position(0)) - last);
    out += "layout(location = " + m[1].str() + ") in " + m[2].str() + " " + m[3].str() + "_lkIn; " + m[2].str() + " " + m[3].str() + ";";
    last = size_t(m.position(0) + m.length(0));
    ++found;
  }
  out += vs.substr(last);
  if (found != 4) { *why = "chunk.vert does not declare aPos/aInfo/aShade/aLayerHi as plain attributes"; return std::nullopt; }
  static const std::regex mainRe(R"(void\s+main\s*\(\s*(void)?\s*\))");
  std::smatch mm;
  if (!std::regex_search(out, mm, mainRe)) { *why = "no main() in chunk.vert"; return std::nullopt; }
  out.replace(size_t(mm.position(0)), size_t(mm.length(0)), "void lk_chunk_main()");
  const std::optional<std::string> hdr = builtinFile("canonical_pull_legacy.glsl");
  if (!hdr) { *why = "missing built-in canonical_pull_legacy.glsl"; return std::nullopt; }
  out += "\n" + *hdr + R"(
uniform usamplerBuffer uCanonTbo;
uniform int uCanonMode; uniform int uCanonBase; uniform int uCanonFb; uniform int uCanonLod;
void main() {
  if (uCanonMode == 0) { aPos = aPos_lkIn; aInfo = aInfo_lkIn; aShade = aShade_lkIn; aLayerHi = aLayerHi_lkIn; }
  else {
    int q = gl_VertexID / 6, k = gl_VertexID - q * 6;
    uint corner = uint(k < 3 ? k : (k == 3 ? 0 : k - 2));  // the shared quad pattern {0,1,2,0,2,3}
    CanonLegacyVertex cv;
    if (q < uCanonFb) cv = canonPullFallback(texelFetch(uCanonTbo, uCanonBase + q * 4 + int(corner)));
    else cv = canonPullQuadLegacy(texelFetch(uCanonTbo, uCanonBase + uCanonFb * 4 + (q - uCanonFb)), corner, uint(uCanonLod));
    aPos = cv.pos; aInfo = cv.info; aShade = cv.shade; aLayerHi = cv.layerHi;
  }
  lk_chunk_main();
}
)";
  return out;
}

constexpr uint32_t kMaxQuads = kMaxQuadsPerMesh;  // shared quad index buffer size (renderer.h)

class cGL33Renderer final : public IRenderer {
 public:
  // Ginny 2026-10-16 (perf list): greedy-merged chunk faces; chunk_shading.glsl repeats the tile.
  bool prefersGreedyMeshes() const override { return true; }
  explicit cGL33Renderer(bool arena) : useArena_(arena) {}
  bool init() {
    logInfo("gl33: chunk meshes in %s", useArena_ ? "one arena VBO + VAO (rebased-index draws)" : "one VBO + VAO per sub-chunk");
    if (loadShaders(ShaderLibrary{}) < 0) return false;  // built-ins until the app loads pack shaders
    std::vector<uint32_t> idx(kMaxQuads * 6);  // shared quad index buffer
    for (uint32_t q = 0; q < kMaxQuads; ++q) {
      const uint32_t b = q * 4; uint32_t* o = &idx[q * 6];
      o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
    }
    glGenBuffers(1, &ebo_); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
    if (useArena_) {  // Phase 2: per-frame instance records (19 floats per visible mesh)
      glGenBuffers(1, &instVbo_); glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
      glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(instVboCapacityFloats_ * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    }
    glGenVertexArrays(1, &lineVao_); glBindVertexArray(lineVao_);
    glGenBuffers(1, &lineVbo_); glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, 24 * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    initHud();
    // Sprite pass VAO/VBO (streamed per frame by drawSprites).
    glGenVertexArrays(1, &spriteVao_); glBindVertexArray(spriteVao_);
    glGenBuffers(1, &spriteVbo_); glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(SpriteVert) * 6144), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), reinterpret_cast<const void*>(offsetof(SpriteVert, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), reinterpret_cast<const void*>(offsetof(SpriteVert, r)));
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
    return true;
  }

  void initHud() {
    const HudFontAtlas atlas = buildHudFontAtlas();
    glGenTextures(1, &fontTex_); glBindTexture(GL_TEXTURE_2D, fontTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas.width, atlas.height, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &hudVao_); glBindVertexArray(hudVao_);
    glGenBuffers(1, &hudVbo_); glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
    const GLsizei stride = sizeof(HudVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HudVertex, u)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HudVertex, r)));
  }

  void drawHud(const FrameParams& p) {
    buildHudVertices(p.hud, hudVerts_);
    if (hudVerts_.empty()) return;
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);  // pixel-space quads are clockwise in NDC
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hudProg_.use();
    glUniform2f(uScreen_, float(p.width), float(p.height));
    glBindTexture(GL_TEXTURE_2D, fontTex_);
    glBindVertexArray(hudVao_); glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
    // Q3 (Archie exec summary, 2026-10-28): same per-frame orphan
    // pattern as the mega instance buffer... grow-only allocation,
    // BufferSubData per frame.
    if (hudVerts_.size() > hudVboCapacityVerts_) {
      hudVboCapacityVerts_ = std::max(hudVerts_.size(), hudVboCapacityVerts_ * 2);
      glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(hudVboCapacityVerts_ * sizeof(HudVertex)), nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(hudVerts_.size() * sizeof(HudVertex)), hudVerts_.data());
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(hudVerts_.size()));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE);
  }

  // Sprite atlas pages: (re)upload when the game's registry version moved.
  // Pages upload row-first (Image row order), which pairs with the emitted
  // image-space UVs exactly (see renderer.h SpriteDrawQuad).
  void uploadSpriteAtlas(const FrameParams& p) {
    for (GLuint t : spriteTex_) glDeleteTextures(1, &t);
    spriteTex_.clear();
    glActiveTexture(GL_TEXTURE0);
    for (const Image& page : p.spriteAtlasPages) {
      GLuint t = 0;
      glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, page.width(), page.height(), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, page.pixels().data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      spriteTex_.push_back(t);
    }
    spriteAtlasUploaded_ = p.spriteAtlasVersion;
  }

  // Sprite pass: FrameParams::spriteQuads after the world + entities, before
  // lines/HUD. Quads arrive pre-ordered (cutout, then blended back-to-front),
  // so this is a single streamed VBO drawn in runs of equal (shade, page),
  // switching blend/depth state per run. Sprites are double-sided (a
  // FlatFacing decal reads from both sides), so culling is off for the pass.
  void drawSprites(const FrameParams& p) {
    if (!spriteProg_.id() || !p.spriteQuads || p.spriteQuadCount == 0) return;
    if (p.spriteAtlasPages.empty()) return;
    if (p.spriteAtlasVersion != spriteAtlasUploaded_) uploadSpriteAtlas(p);
    if (spriteTex_.empty()) return;
    spriteVerts_.clear();
    spriteVerts_.reserve(p.spriteQuadCount * 6);
    for (size_t q = 0; q < p.spriteQuadCount; ++q) {
      const SpriteDrawQuad& s = p.spriteQuads[q];
      static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
      for (int vi : kOrder) {
        const Vec3& c = s.corners[vi];
        spriteVerts_.push_back(SpriteVert{c.x, c.y, c.z, s.uv[vi][0], s.uv[vi][1], s.r, s.g, s.b, s.a});
      }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(spriteVao_); glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    // Grow-only streaming (P3-3, Appendix D M5): the old code called
    // glBufferData with the exact frame size EVERY frame, orphaning and
    // reallocating the store each time (the 6144-vert init allocation was
    // discarded on the first frame). Now: glBufferData only when the
    // frame outgrows capacity; glBufferSubData into the existing store
    // otherwise.
    if (spriteVerts_.size() > spriteVboCapacity_) {
      spriteVboCapacity_ = spriteVerts_.size();
      glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(spriteVboCapacity_ * sizeof(SpriteVert)),
                   spriteVerts_.data(), GL_DYNAMIC_DRAW);
    } else if (!spriteVerts_.empty()) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(spriteVerts_.size() * sizeof(SpriteVert)),
                      spriteVerts_.data());
    }
    spriteProg_.use();
    glUniformMatrix4fv(uSpriteVP_, 1, GL_FALSE, p.viewProj.m);
    glDisable(GL_CULL_FACE);
    size_t i = 0;
    while (i < p.spriteQuadCount) {
      const SpriteDrawQuad& s = p.spriteQuads[i];
      size_t j = i + 1;
      while (j < p.spriteQuadCount && p.spriteQuads[j].shade == s.shade &&
             p.spriteQuads[j].page == s.page)
        ++j;
      if (s.page >= 0 && size_t(s.page) < spriteTex_.size()) {
        if (s.shade == kShadeCutout) {
          glDisable(GL_BLEND); glDepthMask(GL_TRUE);
          glUniform1f(uSpriteAlphaTest_, 0.5f);
        } else if (s.shade == kShadeAdditive) {
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glDepthMask(GL_FALSE);
          glUniform1f(uSpriteAlphaTest_, 0.f);
        } else {  // kShadeAlphaBlend
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          glDepthMask(GL_FALSE);
          glUniform1f(uSpriteAlphaTest_, 0.f);
        }
        glBindTexture(GL_TEXTURE_2D, spriteTex_[size_t(s.page)]);
        glDrawArrays(GL_TRIANGLES, GLsizei(i * 6), GLsizei((j - i) * 6));
      }
      i = j;
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_CULL_FACE);
    ++stats_.drawn;
  }

  ~cGL33Renderer() override {
    clearChunkMeshes();
    glDeleteTextures(1, &fontTex_);
    if (pbrMatTex_) glDeleteTextures(1, &pbrMatTex_);
    if (pbrMatBuf_) glDeleteBuffers(1, &pbrMatBuf_);
    if (sceneColorTex_) glDeleteTextures(1, &sceneColorTex_);
    // Fix 2026-10-11 (audit): these 7 were never deleted.
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (lineVao_) glDeleteVertexArrays(1, &lineVao_);
    if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
    if (entityVao_) glDeleteVertexArrays(1, &entityVao_);
    if (entityVbo_) glDeleteBuffers(1, &entityVbo_);
    if (hudVao_) glDeleteVertexArrays(1, &hudVao_);
    if (hudVbo_) glDeleteBuffers(1, &hudVbo_);
    if (spriteVao_) glDeleteVertexArrays(1, &spriteVao_);
    if (spriteVbo_) glDeleteBuffers(1, &spriteVbo_);
    for (GLuint t : spriteTex_) glDeleteTextures(1, &t);
  }

  const char* name() const override { return "OpenGL 3.3 core"; }
  ITextureArray& textureArray() override { return textures_; }

  void setMaterials(const BlockMaterials& m) override {
    bindings_ = m.bindings();
    animations_.assign(m.animations().begin(), m.animations().end());
    animFrames_.assign(m.animationFrames().begin(), m.animationFrames().end());
    applyMaterialUniforms();
  }

  // ---- PBR (BinaryQuad) path (phase 2, Worker 3) ----
  bool supportsPbrPath() const override { return pbrProg_.id() != 0; }

  void setPbrMaterials(const cMaterialReg& reg) override {
    if (!supportsPbrPath()) return;
    const uint64_t gen = reg.generation();
    if (gen == pbrMatGeneration_) return;  // table is current; uploads are rare
    std::vector<PbrMaterialRow> rows;
    packPbrMaterialTable(reg, rows);  // 48-byte std430 rows, indexed by compact ID
    if (!pbrMatBuf_) { glGenBuffers(1, &pbrMatBuf_); glGenTextures(1, &pbrMatTex_); }
    // Buffer texture (GL 3.1+ core): the shader unpacks rows via
    // texelFetch(uPbrMaterials, id * 12 + k). SSBOs would need GL 4.3.
    glBindBuffer(GL_TEXTURE_BUFFER, pbrMatBuf_);
    glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(rows.size() * sizeof(PbrMaterialRow)),
                 rows.empty() ? nullptr : rows.data(), GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, pbrMatTex_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, pbrMatBuf_);
    pbrMatGeneration_ = gen;
    pbrMatRows_ = int(rows.size());
    logInfo("gl33: PBR material table uploaded (%zu materials, generation %llu)",
            rows.size(), (unsigned long long)gen);
  }

  void setPbrTier(PbrTransparencyTier t) override { pbrTier_ = t; }
  void setAmbientMode(AmbientMode m) override { ambientMode_ = m; }
  // PROVISIONAL (JD 2026-10-24): texture-filtering stand-in.
  void setTextureFiltering(int filter, int aniso) override { textures_.setFiltering(filter, aniso); }
  int maxAnisotropy() const override { return const_cast<cGlTextureArray&>(textures_).maxAnisotropy(); }
  void setFrustumMarginDeg(float degrees) override {
    frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11
  }
  void setModelDither(bool enabled, float nearBlocks, float farBlocks) override {
    ditherEnabled_ = enabled;
    ditherNear_ = std::clamp(nearBlocks, 0.f, 1024.f);
    ditherFar_ = std::clamp(farBlocks, ditherNear_, 2048.f);
  }
  void setCullProtoConfig(const CullProtoConfig& cfg) override { cullProto_.configure(cfg); }
  CullProtoStats cullProtoStats() const override { return cullProto_.stats(); }

  // TrueRefraction (Ultra) scene-color pass (phase 2, Worker 4): copies the opaque
  // composite the frame has drawn so far (classic chunks, PBR opaque pass, entities -
  // but NOT overlay lines, the crosshair, or the HUD, which draw later) into a
  // full-res RGBA8 texture and binds it as uSceneColor for the refractive pass.
  //
  // Decisions (Worker 4's to lock):
  // - Full-res, not half-res: the refraction math needs screen-space UV accuracy
  //   (grazing rays offset by whole texels at half-res); Ultra is the opt-in quality
  //   tier, so the fill cost is the accepted price. Half-res stays a valid future
  //   optimization if Ultra ever needs a perf mode.
  // - Bilinear + mip chain for the roughness blur: one textureLod() sample with
  //   lod = roughness * 4 (1/16-res at roughness 1) is the honest cheap PBR answer -
  //   far cheaper than a multi-tap gather, and the mip build is one
  //   glGenerateMipmap per frame at tier 3 only.
  // - The copy is sRGB-encoded framebuffer output; the shader linearizes the sample
  //   (pbrSrgbToLinear) before the Fresnel split.
  // Requires pbrProg_ to be current on entry (sets its uniforms); leaves the scene
  // texture bound on kSceneColorTexUnit.
  void copySceneColor(const FrameParams& p) {
    const int w = p.width, h = p.height;
    if (w <= 0 || h <= 0) return;
    if (sceneColorTex_ == 0) glGenTextures(1, &sceneColorTex_);
    glActiveTexture(GL_TEXTURE0 + kSceneColorTexUnit);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    if (w != sceneColorW_ || h != sceneColorH_) {
      sceneColorW_ = w;
      sceneColorH_ = h;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    // Copy from the default framebuffer's back buffer (everything drawn so far).
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glGenerateMipmap(GL_TEXTURE_2D);  // roughness blur source; tier-3-only cost
    pbrProg_.use();
    glUniform1i(uSceneColor_, kSceneColorTexUnit);
    glUniform2f(uSceneSize_, float(w), float(h));
    glUniformMatrix4fv(uSceneVP_, 1, GL_FALSE, p.viewProj.m);
  }

  void uploadPbrChunkMesh(const PbrChunkMesh& m) override {
    const int64_t key = (int64_t(m.cx) << 40) ^ (int64_t(m.sy) << 32) ^ int64_t(uint32_t(m.cz));
    if (m.verts.empty() || !supportsPbrPath()) { erasePbr(key); return; }
    // PBR meshes deliberately stay OUT of the chunk arena: the arena's stride and
    // attribute assumptions are PackedVertex-specific, and PBR meshes are the
    // minority path (opaque cubes only; everything else stays classic).
    PbrGpu& g = pbrMeshes_[key];
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(m.verts.size() / 4, kMaxQuadsPerMesh) * 6);
    if (!g.vao) {
      glGenVertexArrays(1, &g.vao); glBindVertexArray(g.vao);
      glGenBuffers(1, &g.vbo); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
      glEnableVertexAttribArray(0);  // aPos: uvec3, 1/16 block units
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PbrQuadVertex), nullptr);
      glEnableVertexAttribArray(1);  // aFace: uint
      glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(8));
      glEnableVertexAttribArray(2);  // aMaterial: uint (compact registry ID)
      glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(12));
      glEnableVertexAttribArray(3);  // aLight: uvec2 (block/sky 0..64)
      glVertexAttribIPointer(3, 2, GL_UNSIGNED_BYTE, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(16));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);  // shared quad index buffer
    } else {
      glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    }
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.verts.size() * sizeof(PbrQuadVertex)), m.verts.data(), GL_STATIC_DRAW);
  }

  // Returns how many programs use pack sources, or -1 if even a built-in program failed.
  // chunk_pbr is the exception: it may fail (old driver, pack override broken) without
  // failing init - the PBR path then reports unsupported and the game falls back to
  // the classic PackedVertex path (documented activation order, Worker 3).
  int loadShaders(const ShaderLibrary& lib) override {
    // Build every program first and swap them in only if all succeeded, so a failure never leaves
    // a mix of old and new programs bound.
    int fromPacks = 0;
    std::array<std::optional<cGlProgram>, std::size(kProgramNames)> built;
    for (size_t i = 0; i < std::size(kProgramNames); ++i) {
      const std::string_view name = kProgramNames[i];
      const bool pbr = (i == kChunkPbrProgram);
#if defined(__wasm__)
      // fix 3b (Elfie P1): chunk_pbr.frag uses samplerBuffer, which GLSL ES 3.00 lacks. Decline the
      // PBR path up front with one info line instead of a compile ERROR (supportsPbrPath() -> false).
      if (pbr) { logInfo("gl33: chunk_pbr not available on WebGL2 (no samplerBuffer in GLSL ES 3.00); classic path only"); continue; }
#endif
      std::string log;
      std::optional<cGlProgram>& prog = built[i];
      if (const ShaderSource* src = lib.find(name)) {
        prog = cGlProgram::build(src->vertex, src->fragment, &log);
        if (prog) ++fromPacks;
        else logError("shader '%.*s' from %s failed, using the built-in copy:\n%s", int(name.size()), name.data(), src->origin.c_str(), log.c_str());
      } else if (lib.size() > 0) {
        logInfo("shader '%.*s': not in any pack, using the built-in copy", int(name.size()), name.data());
      }
      if (!prog) {
        const std::string file(name);
        const std::string vs = builtinSource(file + ".vert", &log), fs = builtinSource(file + ".frag", &log);
        if (!vs.empty() && !fs.empty()) prog = cGlProgram::build(vs, fs, &log);
        if (!prog) {
          logError("built-in shader '%s' failed:\n%s", file.c_str(), log.c_str());
          if (pbr || i == kSpriteProgram) continue;  // optional path unsupported; the rest still inits
          return -1;
        }
      }
    }
    for (size_t i = 0; i < std::size(kProgramNames); ++i) {
      if (i == kChunkPbrProgram || i == kSpriteProgram) continue;
      const std::string_view name = kProgramNames[i];
      (name == "chunk" ? prog_ : name == "hud" ? hudProg_ : lineProg_) = std::move(*built[i]);
    }
    // PBR program: swap in on success, clear on failure (supportsPbrPath() reads id()).
    if (built[kChunkPbrProgram]) pbrProg_ = std::move(*built[kChunkPbrProgram]);
    else pbrProg_ = cGlProgram();
#if !defined(__wasm__)  // fix 3b: on WebGL2 the expected decline was logged once (info) above
    if (!supportsPbrPath()) logError("gl33: chunk_pbr unavailable - PBR/BinaryQuad path disabled, classic path only");
#endif
    // Sprite program: same optional-path treatment (drawSprites() reads id()).
    if (built[kSpriteProgram]) spriteProg_ = std::move(*built[kSpriteProgram]);
    else spriteProg_ = cGlProgram();
    if (!spriteProg_.id()) logError("gl33: sprite shader unavailable - sprite pass disabled");
    else {
      uSpriteVP_ = spriteProg_.uniform("uVP");
      uSpriteAtlas_ = spriteProg_.uniform("uAtlas");
      uSpriteAlphaTest_ = spriteProg_.uniform("uAlphaTest");
      spriteProg_.use();
      glUniform1i(uSpriteAtlas_, 0);  // atlas pages live on texture unit 0 (2D target)
    }
#if !defined(__wasm__)
    canonOn_ = false;
    if (const char* e = std::getenv("LL_GL_CANON"); e && e[0] == '1') {
      std::string clog, vs, fs;
      if (const ShaderSource* src = lib.find("chunk")) { vs = std::string(src->vertex); fs = std::string(src->fragment); }
      else { vs = builtinSource("chunk.vert", &clog); fs = builtinSource("chunk.frag", &clog); }
      std::optional<std::string> cvs = vs.empty() ? std::nullopt : canonPullVariant(vs, &clog);
      std::optional<cGlProgram> cp = cvs ? cGlProgram::build(*cvs, fs, &clog) : std::nullopt;
      if (cp) {
        prog_ = std::move(*cp);
        const char* sl = std::getenv("LL_GL_CANON_SLABS");
        canonOn_ = canonUp_.pool().init(sl ? std::atoi(sl) : 2);  // fix (c): 64 MiB slabs, preallocated
        if (canonOn_ && !canonVao_) glGenVertexArrays(1, &canonVao_);
        uCanonMode_ = prog_.uniform("uCanonMode"); uCanonBase_ = prog_.uniform("uCanonBase");
        uCanonFb_ = prog_.uniform("uCanonFb"); uCanonLod_ = prog_.uniform("uCanonLod");
        prog_.use();
        glUniform1i(prog_.uniform("uCanonTbo"), kCanonTexUnit);
        glUniform1i(uCanonMode_, 0);
        canonModeSet_ = 0;
      }
      logInfo("gl33: canonical TBO uploader (LL_GL_CANON) %s%s", canonOn_ ? "ON (RGBA32UI pull stream, legacy fallback per mesh)" : "OFF: ",
              canonOn_ ? "" : clog.c_str());
    }
#endif
    uVP_ = prog_.uniform("uVP"); uOffset_ = prog_.uniform("uOffset");
    uCam_ = prog_.uniform("uCam"); uSky_ = prog_.uniform("uSky");
    uFogEnd_ = prog_.uniform("uFogEnd"); uTime_ = prog_.uniform("uTime"); uTwinkle_ = prog_.uniform("uTwinkle"); uDebug_ = prog_.uniform("uDebug");
    uModelDither_ = prog_.uniform("uModelDither"); uModelDitherBand_ = prog_.uniform("uModelDitherBand");
    uPulseP0_ = prog_.uniform("uPulseP0"); uPulseP1_ = prog_.uniform("uPulseP1");
    uGroundPulseTime_ = prog_.uniform("uGroundPulseTime"); uGroundPulseOn_ = prog_.uniform("uGroundPulseOn");
    uTraceP0_ = prog_.uniform("uTraceP0"); uTraceP1_ = prog_.uniform("uTraceP1");  // wafer ground traces (JD 2026-10-11)
    uGroundTraceTime_ = prog_.uniform("uGroundTraceTime"); uGroundTraceOn_ = prog_.uniform("uGroundTraceOn");
    uLineM_ = lineProg_.uniform("uM"); uLineC_ = lineProg_.uniform("uC");
    // Phase 2 mega-draw variant of the chunk program: same sources with
    // LK_MEGADRAW injected after the #version line; per-mesh offset/pulse/
    // trace then arrive as instanced attributes + flat varyings instead of
    // uniforms. Built from whichever source prog_ came from (pack library
    // first, built-in copy otherwise). Any failure leaves megaOk_ false and
    // the renderer behaves exactly as before Phase 2.
    progMega_ = cGlProgram();
    megaOk_ = false;
    {
      std::string mlog;
      std::string vs, fs;
      if (const ShaderSource* src = lib.find("chunk")) { vs = std::string(src->vertex); fs = std::string(src->fragment); }
      else { vs = builtinSource("chunk.vert", &mlog); fs = builtinSource("chunk.frag", &mlog); }
      auto inject = [](std::string s) {
        const size_t nl = s.find('\n');
        if (nl != std::string::npos) s.insert(nl + 1, "#define LK_MEGADRAW 1\n");
        return s;
      };
      if (!vs.empty() && !fs.empty()) {
        if (auto mp = cGlProgram::build(inject(vs), inject(fs), &mlog)) {
          progMega_ = std::move(*mp);
          uMegaVP_ = progMega_.uniform("uVP"); uMegaCam_ = progMega_.uniform("uCam");
          uMegaSky_ = progMega_.uniform("uSky"); uMegaFogEnd_ = progMega_.uniform("uFogEnd");
          uMegaTime_ = progMega_.uniform("uTime"); uMegaTwinkle_ = progMega_.uniform("uTwinkle");
          uMegaDebug_ = progMega_.uniform("uDebug");
          uMegaModelDither_ = progMega_.uniform("uModelDither");
          uMegaModelDitherBand_ = progMega_.uniform("uModelDitherBand");
          uMegaGroundPulseTime_ = progMega_.uniform("uGroundPulseTime");
          uMegaGroundPulseOn_ = progMega_.uniform("uGroundPulseOn");
          uMegaGroundTraceTime_ = progMega_.uniform("uGroundTraceTime");
          uMegaGroundTraceOn_ = progMega_.uniform("uGroundTraceOn");
          applyMaterialUniformsTo(progMega_);
        } else {
          logError("gl33: mega-draw chunk program failed to build; mega-draw off:\n%s", mlog.c_str());
        }
      }
      // Capability latch: the thin layer answers 1 only when
      // WEBGL_multi_draw exists on the live context. Native builds have no
      // such verb and keep the per-mesh path.
      megaOk_ = false;
#if defined(__wasm__)
      megaOk_ = useArena_ && progMega_.id() && lk_mega_probe() == 1;
#endif
      logInfo("gl33: mega-draw %s", megaOk_ ? "ON (one instanced multi-draw per frame for classic chunk meshes)" : "off (per-mesh draws)");
      if (useArena_) for (auto& [k, g] : meshes_) buildMeshIndices(g);
    }
    uScreen_ = hudProg_.uniform("uScreen");
    if (supportsPbrPath()) {
      uPbrVP_ = pbrProg_.uniform("uVP"); uPbrOffset_ = pbrProg_.uniform("uOffset");
      uPbrCam_ = pbrProg_.uniform("uCam");
      uPbrMat_ = pbrProg_.uniform("uPbrMaterials");
      uPbrLightPos_ = pbrProg_.uniform("uPbrLightPos[0]");
      uPbrLightColor_ = pbrProg_.uniform("uPbrLightColor[0]");
      uPbrLightCount_ = pbrProg_.uniform("uPbrLightCount");
      uPbrTier_ = pbrProg_.uniform("uPbrTier");
      uPbrModelDither_ = pbrProg_.uniform("uModelDither");
      uPbrModelDitherBand_ = pbrProg_.uniform("uModelDitherBand");
      uAmbientMode_ = pbrProg_.uniform("uAmbientMode");
      uSunDir_ = pbrProg_.uniform("uSunDir");
      uPbrMaterialRows_ = pbrProg_.uniform("uPbrMaterialRows");
      uSceneColor_ = pbrProg_.uniform("uSceneColor");
      uSceneSize_ = pbrProg_.uniform("uSceneSize");
      uSceneVP_ = pbrProg_.uniform("uSceneVP");
      uRefractionPass_ = pbrProg_.uniform("uRefractionPass");
      pbrProg_.use();
      glUniform1i(uPbrMat_, kPbrMatTexUnit);  // material table lives on unit 2 (0/1 are classic)
    }
    hudProg_.use();
    glUniform1i(hudProg_.uniform("uFont"), 0);
    applyMaterialUniforms();
    return fromPacks;
  }

  std::string deviceInfo() const override {
    auto s = [](GLenum e) { const unsigned char* p = glGetString(e); return std::string(p ? reinterpret_cast<const char*>(p) : "?"); };
    return s(GL_VENDOR) + " | " + s(GL_RENDERER) + " | " + s(GL_VERSION);
  }

  void setEntityOrigin(float x, float y, float z) override { entityOrigin_[0] = x; entityOrigin_[1] = y; entityOrigin_[2] = z; }  // GAME-H1
  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override {
    // Dynamic VBO for entities (items, etc.). Rebuilt each frame.
    // Binds exactly what chunk.vert declares (drawEntities uses prog_):
    // loc 0 = aPos @0, loc 1 = aInfo @6, loc 2 = aShade @10.
    // (Fix 2026-10-11: was binding slot@6 as uint to loc 1 and u,v,faceAo,layer
    // to loc 2 — neither matched the shader.)
    if (!entityVao_) {
      glGenVertexArrays(1, &entityVao_); glBindVertexArray(entityVao_);
      glGenBuffers(1, &entityVbo_); glBindBuffer(GL_ARRAY_BUFFER, entityVbo_);
      glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex) * 4096, nullptr, GL_DYNAMIC_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
      glEnableVertexAttribArray(2);
      glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
    glEnableVertexAttribArray(3);  // Cortana 2026-10-15 1628: CA-03 [ENGINE] aLayerHi (byte 15)
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(15));
      glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, entityVbo_);
    const size_t n = std::min(verts.size(), size_t(4096));
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(PackedVertex), verts.data());
    entityVertCount_ = GLsizei(n);
  }
  void uploadChunkMesh(const ChunkMesh& m) override {
    // Fix 2026-10-11 (regression): left-shifting negative int64_t is UB;
    // cast through uint64_t first, then back (bit-preserving). Mirrors vk_renderer.cpp.
    const int64_t key = int64_t((uint64_t(uint32_t(m.cx)) << 40) ^ (uint64_t(uint32_t(m.sy)) << 32) ^ uint64_t(uint32_t(m.cz)));
    if (m.verts.empty()) { erase(key); return; }
    static const bool kCanonNoUp = [] { const char* e = std::getenv("LL_GL_CANON_NOUP"); return e && e[0] == '1'; }();  // diag: wrapped shader, arrays only
    if (canonOn_ && !kCanonNoUp && m.canonical && uploadCanon(key, m)) return;
    if (auto it = meshes_.find(key); it != meshes_.end() && it->second.canon) erase(key);  // canonical -> legacy
    if (useArena_) { uploadToArena(key, m); return; }
    Gpu& g = meshes_[key];
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(m.verts.size() / 4, kMaxQuads) * 6);  // the mesher already caps
    for (int i = 0; i < 4; ++i) { g.pulseP0[i] = m.pulseP0[i]; g.pulseP1[i] = m.pulseP1[i]; g.traceP0[i] = m.traceP0[i]; g.traceP1[i] = m.traceP1[i]; }
    if (!g.vao) {
      glGenVertexArrays(1, &g.vao); glBindVertexArray(g.vao);
      glGenBuffers(1, &g.vbo); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
      // Fix 2026-10-11 (regression): the CRITICAL PackedVertex fix covered the
      // entity VAO and arena VAO but missed this per-chunk non-arena VAO, which
      // still bound the pre-fix layout (slot uint@6). Binds exactly what
      // chunk.vert declares, like the entity VAO above:
      // loc 0 = aPos @0, loc 1 = aInfo @6, loc 2 = aShade @10.
      glEnableVertexAttribArray(0);
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
      glEnableVertexAttribArray(2);
      glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
    glEnableVertexAttribArray(3);  // Cortana 2026-10-15 1628: CA-03 [ENGINE] aLayerHi (byte 15)
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(15));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    } else {
      glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    }
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.verts.size() * sizeof(PackedVertex)), m.verts.data(), GL_STATIC_DRAW);
  }

  // M12: one RGBA32UI buffer texture holds every canonical mesh's pull stream (first-fit
  // free list in texel units, doubling growth like the vertex arena).
  // M12 canonical pull stream via the IMeshUploader (refactor step 3): prepare (build +
  // validate), drop the old entry, commit (slab placement + upload). Same order as before.
  bool uploadCanon(int64_t key, const ChunkMesh& m) {
    if (!canonUp_.prepare(m)) return false;
    erase(key);
    MeshPlacement pl;
    if (!canonUp_.commit(pl)) return false;
    Gpu& g = meshes_[key];
    g.canon = true; g.cSlab = pl.slab; g.cFirst = pl.first; g.cCount = pl.count; g.cFb = GLint(pl.fallbackQuads);
    g.cVerts = GLsizei(size_t(pl.fallbackQuads + pl.quads) * 6);
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    for (int i = 0; i < 4; ++i) { g.pulseP0[i] = m.pulseP0[i]; g.pulseP1[i] = m.pulseP1[i]; g.traceP0[i] = m.traceP0[i]; g.traceP1[i] = m.traceP1[i]; }
    return true;
  }
  void clearChunkMeshes() override {
    canonUp_.clear();
    for (auto& [k, g] : meshes_) if (g.vao) { glDeleteBuffers(1, &g.vbo); glDeleteVertexArrays(1, &g.vao); }
    meshes_.clear();
    for (auto& [k, g] : pbrMeshes_) if (g.vao) { glDeleteBuffers(1, &g.vbo); glDeleteVertexArrays(1, &g.vao); }
    pbrMeshes_.clear();
    if (arenaVao_) { glDeleteBuffers(1, &arenaVbo_); glDeleteVertexArrays(1, &arenaVao_); }
    arenaVao_ = arenaVbo_ = 0;
    arenaVerts_ = 0;
    arenaFree_.clear();
    if (arenaEbo_) { glDeleteBuffers(1, &arenaEbo_); arenaEbo_ = 0; }
    if (instVbo_) { glDeleteBuffers(1, &instVbo_); instVbo_ = 0; }
    arenaIndices_ = 0;
    arenaIndexFree_.clear();
  }

  void renderFrame(const FrameParams& p) override {
    // Culling-cache prototypes (JD 2026-10-11): arm per-frame state before the
    // chunk loops below. All default off; base margin culling is the foundation.
    cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
    lastFrameW_ = p.width;
    lastFrameH_ = p.height;
    glViewport(0, 0, p.width, p.height);
    glClearColor(p.sky.x, p.sky.y, p.sky.z, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    prog_.use();
    glUniformMatrix4fv(uVP_, 1, GL_FALSE, p.viewProj.m);
    glUniform3f(uCam_, p.camPos.x, p.camPos.y, p.camPos.z);
    glUniform3f(uSky_, p.sky.x, p.sky.y, p.sky.z);
    glUniform1f(uFogEnd_, p.fogEnd);
    glUniform1f(uModelDither_, ditherEnabled_ ? 1.f : 0.f);  // near-camera fade (JD 2026-10-13)
    glUniform2f(uModelDitherBand_, ditherNear_, ditherFar_);
    glUniform1f(uTime_, p.timeSec);
    glUniform1i(uTwinkle_, p.leafTwinkle);  // 2026-10-16 crystal twinkle (render clock)
    glUniform1i(uDebug_, p.debugOverlay ? 1 : 0);
    glUniform1f(uGroundPulseTime_, p.groundPulseTimeSec);  // wafer ground pulse: sim time, pausing freezes it
    glUniform1f(uGroundPulseOn_, p.groundPulseOn ? 1.f : 0.f);
    glUniform1f(uGroundTraceTime_, p.groundTraceTimeSec);  // wafer ground traces: same sim clock, independent toggle
    glUniform1f(uGroundTraceOn_, p.groundTraceOn ? 1.f : 0.f);
    textures_.bind();
    canonBoundSlab_ = ~uint32_t(0);  // fix (c): the slab TBO is bound per draw, on change
    stats_.drawn = 0;
    // Fix 2026-10-11 (audit): was classic meshes only; count PBR meshes too.
    stats_.meshes = uint32_t(meshes_.size() + pbrMeshes_.size());
    GLuint boundVao = 0;
    // Cull decisions are made ONCE per chunk per frame (P3-3, Appendix D
    // L5): decide() has stats/state side effects, and the PBR loop below
    // used to re-decide chunks present in both maps — double-counting
    // cullProtoStats and re-running the comet/velocity state. The PBR
    // lambda (and both TrueRefraction passes) reuse this frame's map.
    std::unordered_map<int64_t, CullDecision> frameCull_;
    frameCull_.reserve(meshes_.size() + pbrMeshes_.size());  // pacing pass 2026-10-19: one bucket array per frame, no rehash storm
    auto decideOnce = [&](int64_t key, const Vec3& mn) {
      const auto it = frameCull_.find(key);
      if (it != frameCull_.end()) return it->second;
      const CullDecision cd =
          cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos, frustumMarginDeg_);
      frameCull_.emplace(key, cd);
      return cd;
    };
    // Pacing pass (2026-10-19): the four per-mesh ground-effect uniforms
    // (wafer ground pulse P0/P1, ground traces P0/P1) repeat identically
    // across most meshes; re-uploading an identical value is a pure state
    // no-op, so repeats within the frame are skipped (uOffset_ differs per
    // mesh and is always set). The GL state after every mesh is bit-exact
    // what the un-skipped sequence would leave — pixels cannot move.
    float lastPulseP0[4] = {}, lastPulseP1[4] = {}, lastTraceP0[4] = {}, lastTraceP1[4] = {};
    bool effectPrimed = false;
    auto setEffect4 = [&](GLint loc, float last[4], const float v[4]) {
      if (!effectPrimed || std::memcmp(last, v, sizeof(float) * 4) != 0) {
        glUniform4fv(loc, 1, v);
        std::memcpy(last, v, sizeof(float) * 4);
      }
    };
    // Phase 2 mega-draw (2026-10-23): when latched on, the whole visible
    // classic-mesh list goes out as ONE instanced multi-draw — per-mesh data
    // (offset + pulse/trace) rides the instance buffer, indices come from the
    // arena index buffer, and no per-mesh uniform or VAO call crosses the
    // bridge. Cull decisions are the same decideOnce() calls the classic
    // loop makes, so the visible set is identical by construction.
    bool megaDrew = false;
    if (megaOk_) {
      megaCounts_.clear(); megaOffsets_.clear(); megaData_.clear();
      bool megaComplete = true;
      for (const auto& [k, g] : meshes_) {
        const Vec3 mn{g.cx * 16.f, g.sy * 16.f, g.cz * 16.f};
        if (decideOnce(k, mn) != CullDecision::Full) continue;
        if (g.indexBuiltCount <= 0) { megaComplete = false; break; }  // never partially mega-draw: classic loop draws the whole frame
        megaCounts_.push_back(g.indexCount);
        megaOffsets_.push_back(GLint(g.indexFirst * sizeof(uint32_t)));
        const float off[3] = {mn.x, mn.y, mn.z};
        megaData_.insert(megaData_.end(), off, off + 3);
        megaData_.insert(megaData_.end(), g.pulseP0, g.pulseP0 + 4);
        megaData_.insert(megaData_.end(), g.pulseP1, g.pulseP1 + 4);
        megaData_.insert(megaData_.end(), g.traceP0, g.traceP0 + 4);
        megaData_.insert(megaData_.end(), g.traceP1, g.traceP1 + 4);
      }
      if (megaComplete && !megaCounts_.empty()) {
        progMega_.use();
        glUniformMatrix4fv(uMegaVP_, 1, GL_FALSE, p.viewProj.m);
        glUniform3f(uMegaCam_, p.camPos.x, p.camPos.y, p.camPos.z);
        glUniform3f(uMegaSky_, p.sky.x, p.sky.y, p.sky.z);
        glUniform1f(uMegaFogEnd_, p.fogEnd);
        glUniform1f(uMegaModelDither_, ditherEnabled_ ? 1.f : 0.f);
        glUniform2f(uMegaModelDitherBand_, ditherNear_, ditherFar_);
        glUniform1f(uMegaTime_, p.timeSec);
        glUniform1i(uMegaTwinkle_, p.leafTwinkle);
        glUniform1i(uMegaDebug_, p.debugOverlay ? 1 : 0);
        glUniform1f(uMegaGroundPulseTime_, p.groundPulseTimeSec);
        glUniform1f(uMegaGroundPulseOn_, p.groundPulseOn ? 1.f : 0.f);
        glUniform1f(uMegaGroundTraceTime_, p.groundTraceTimeSec);
        glUniform1f(uMegaGroundTraceOn_, p.groundTraceOn ? 1.f : 0.f);
        if (boundVao != arenaVao_) { glBindVertexArray(arenaVao_); boundVao = arenaVao_; }
        glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
        // Q3 (Archie exec summary, 2026-10-28): stream into the
        // pre-allocated instance buffer with BufferSubData. The old
        // exact-size BufferData orphaned and reallocated the buffer
        // EVERY frame... one of the per-frame BufferData crossings
        // convicted by the Build86k device log (~x3/frame). Grow by
        // doubling, only when a frame's visible set outgrows the
        // allocation (the sprite path's house pattern, D-M5).
        if (megaData_.size() > instVboCapacityFloats_) {
          instVboCapacityFloats_ = std::max(megaData_.size(), instVboCapacityFloats_ * 2);
          glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(instVboCapacityFloats_ * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(megaData_.size() * sizeof(float)), megaData_.data());
#if defined(__wasm__)
        lk_mega_draw(GL_TRIANGLES, megaCounts_.data(), megaOffsets_.data(), int(megaCounts_.size()));
#endif
        stats_.drawn += uint32_t(megaCounts_.size());
        megaDrew = true;
      }
    }
    if (!megaDrew) for (const auto& [k, g] : meshes_) {
      const Vec3 mn{g.cx * 16.f, g.sy * 16.f, g.cz * 16.f};
      // Prototype pipeline: Full draws; MeshOnly keeps the uploaded mesh but
      // skips the draw (outer tier); Cull skips. Base margin = foundation.
      const CullDecision cd = decideOnce(k, mn);
      if (cd != CullDecision::Full) continue;
      if (canonOn_ && canonModeSet_ != (g.canon ? 1 : 0)) { canonModeSet_ = g.canon ? 1 : 0; glUniform1i(uCanonMode_, canonModeSet_); }
      glUniform3f(uOffset_, mn.x, mn.y, mn.z);
      setEffect4(uPulseP0_, lastPulseP0, g.pulseP0);  // wafer ground pulse: this mesh's blended params
      setEffect4(uPulseP1_, lastPulseP1, g.pulseP1);
      setEffect4(uTraceP0_, lastTraceP0, g.traceP0);  // wafer ground traces: this mesh's blended params
      setEffect4(uTraceP1_, lastTraceP1, g.traceP1);
      effectPrimed = true;
      if (g.canon) {
        if (boundVao != canonVao_) { glBindVertexArray(canonVao_); boundVao = canonVao_; }
        if (canonBoundSlab_ != g.cSlab) {
          canonBoundSlab_ = g.cSlab;
          glActiveTexture(GL_TEXTURE0 + kCanonTexUnit);
          glBindTexture(GL_TEXTURE_BUFFER, canonUp_.pool().texture(g.cSlab));
          glActiveTexture(GL_TEXTURE0);
        }
        glUniform1i(uCanonBase_, GLint(g.cFirst));
        glUniform1i(uCanonFb_, g.cFb);
        glUniform1i(uCanonLod_, 0);
        glDrawArrays(GL_TRIANGLES, 0, g.cVerts);
      } else if (useArena_) {
        if (boundVao != arenaVao_) { glBindVertexArray(arenaVao_); boundVao = arenaVao_; }
        // Rebased indices (Phase 2): the arena EBO's per-mesh indices already
        // carry the base vertex, so the classic arena draw needs no
        // base-vertex call — which WebGL2 does not have (the reason the web
        // configuration ran with the arena off until now). Identical pixels
        // to the old pattern-EBO + BaseVertex draw.
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(g.indexFirst * sizeof(uint32_t)));
      } else {
        glBindVertexArray(g.vao);
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr);
      }
      ++stats_.drawn;
    }
    if (canonOn_ && canonModeSet_ != 0) { canonModeSet_ = 0; glUniform1i(uCanonMode_, 0); }  // entities etc. use the arrays
    // PBR (BinaryQuad) meshes: drawn with chunk_pbr. The classic meshes above are
    // untouched - program selection follows the uploaded meshes (coexistence).
    // pbrMatTex_ == 0 means no material table was ever uploaded (no registry);
    // without it the shader would read black, so skip the draw (loud, not silent).
    //
    // Draw helpers (lambdas, so the TrueRefraction two-pass path can reuse them):
    // drawPbrMeshes expects pbrProg_ current; drawEntities rebinds the classic prog_.
    auto drawPbrMeshes = [&]() {
      for (const auto& [k, g] : pbrMeshes_) {
        const Vec3 mn{g.cx * 16.f, g.sy * 16.f, g.cz * 16.f};
        if (decideOnce(k, mn) != CullDecision::Full)  // decided once per frame (D-L5)
          continue;
        glUniform3f(uPbrOffset_, mn.x, mn.y, mn.z);
        glBindVertexArray(g.vao);
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr);
        ++stats_.drawn;
      }
    };
    auto drawEntities = [&]() {
      // Entities (items): dynamic VBO, drawn as quads with the classic program.
      if (entityVertCount_ <= 0) return;
      prog_.use();
      glBindVertexArray(entityVao_);
      glUniform3f(uOffset_, entityOrigin_[0], entityOrigin_[1], entityOrigin_[2]);  // GAME-H1
      glDrawArrays(GL_TRIANGLES, 0, entityVertCount_);  // game emits 6 verts per quad
      ++stats_.drawn;
    };
    const bool pbrActive = supportsPbrPath() && !pbrMeshes_.empty() && pbrMatTex_ != 0;
    const bool refraction = pbrActive && pbrTier_ == PbrTransparencyTier::TrueRefraction;
    if (pbrActive) {
      pbrProg_.use();
      glUniformMatrix4fv(uPbrVP_, 1, GL_FALSE, p.viewProj.m);
      glUniform3f(uPbrCam_, p.camPos.x, p.camPos.y, p.camPos.z);
      glActiveTexture(GL_TEXTURE0 + kPbrMatTexUnit);
      glBindTexture(GL_TEXTURE_BUFFER, pbrMatTex_);
      // Analytic lights: the game fills FrameParams::pbrLights per frame from its
      // LightCollector (fed by cChunkEmissiveProvider); empty = specular off.
      const uint32_t nl =
          std::min<uint32_t>(p.pbrLightCount, uint32_t(FrameParams::kMaxPbrFrameLights));
      // Fix 2026-10-11 (audit): skip the array uploads when there are no lights
      // (the common case) — the count uniform alone suffices.
      if (nl > 0) {
        float lightPos[FrameParams::kMaxPbrFrameLights * 4];
        float lightColor[FrameParams::kMaxPbrFrameLights * 3];
        for (uint32_t i = 0; i < nl; ++i) {
          const AnalyticLight& L = p.pbrLights[i];
          lightPos[i * 4 + 0] = L.x; lightPos[i * 4 + 1] = L.y; lightPos[i * 4 + 2] = L.z;
          lightPos[i * 4 + 3] = L.intensity;
          lightColor[i * 3 + 0] = L.r; lightColor[i * 3 + 1] = L.g; lightColor[i * 3 + 2] = L.b;
        }
        glUniform4fv(uPbrLightPos_, GLsizei(nl), lightPos);
        glUniform3fv(uPbrLightColor_, GLsizei(nl), lightColor);
      }
      glUniform1i(uPbrLightCount_, GLint(nl));
      glUniform1i(uPbrTier_, GLint(int(pbrTier_)));
      glUniform1f(uPbrModelDither_, ditherEnabled_ ? 1.f : 0.f);
      glUniform2f(uPbrModelDitherBand_, ditherNear_, ditherFar_);
      glUniform1i(uAmbientMode_, GLint(int(ambientMode_)));
      glUniform3f(uSunDir_, sunDir_[0], sunDir_[1], sunDir_[2]);
      glUniform1i(uPbrMaterialRows_, GLint(pbrMatRows_));
      glUniform1i(uRefractionPass_, 0);  // single-pass tiers ignore it; tier 3 pass 0 builds the copy
      // Blend state follows the tier: Simple and PbrReflections composite by alpha;
      // ScreenDoor discards in-shader (no blending, no sorting); TrueRefraction
      // resolves to an opaque composite in two passes (no blending either pass).
      const bool pbrBlend = pbrTier_ == PbrTransparencyTier::Simple ||
                            pbrTier_ == PbrTransparencyTier::PbrReflections;
      if (pbrBlend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
      if (refraction) {
        // Pass 0: opaque PBR fragments only (the shader discards alpha < 1).
        drawPbrMeshes();
        // Entities join the opaque composite so refractive surfaces show them.
        drawEntities();
        copySceneColor(p);
        // Pass 1: refractive fragments only, sampling the copy (alpha >= 1 discarded).
        pbrProg_.use();
        glUniform1i(uRefractionPass_, 1);
        drawPbrMeshes();
      } else {
        drawPbrMeshes();
        drawEntities();
      }
      if (pbrBlend) glDisable(GL_BLEND);
      glActiveTexture(GL_TEXTURE0);  // leave the classic unit current
    } else {
      drawEntities();
    }
    drawSprites(p);  // sprite pass: after world + entities, before lines/HUD
    lineProg_.use();
    glBindVertexArray(lineVao_); glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    if (p.highlight) {
      const auto v = blockOutlineLines(p.hx, p.hy, p.hz);
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v.data());  // Q3: lineVbo_ is pre-allocated for exactly this fixed-size set
      glUniformMatrix4fv(uLineM_, 1, GL_FALSE, p.viewProj.m);
      glUniform3f(uLineC_, 0.05f, 0.05f, 0.08f);
      glDrawArrays(GL_LINES, 0, GLsizei(v.size()));
    }
    if (p.crosshair && p.height > 0) {
      const auto v = crosshairLines(p.width, p.height);
      const Mat4 id = Mat4::identity();
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v.data());  // Q3: fits the lineVbo_ pre-allocation
      glUniformMatrix4fv(uLineM_, 1, GL_FALSE, id.m);
      glUniform3f(uLineC_, 1.f, 1.f, 1.f);
      glDisable(GL_DEPTH_TEST);
      glDrawArrays(GL_LINES, 0, GLsizei(v.size()));
      glEnable(GL_DEPTH_TEST);
    }
    drawHud(p);
  }

  RenderStats stats() const override { return stats_; }

  // Screenshot grab (lithoscript `screenshot`): the game arms the grab during
  // update, before the frame is drawn; the capture runs after renderFrame(),
  // before the caller presents, reading the back buffer with glReadPixels.
  void requestGrab() override { grabArmed_ = true; }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (!grabArmed_ || lastFrameW_ <= 0 || lastFrameH_ <= 0) return false;
    grabArmed_ = false;
    w = lastFrameW_;
    h = lastFrameH_;
    rgb.resize(size_t(w) * size_t(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#if defined(__wasm__)
    // WebGL2 (ANGLE) robust readback: the default framebuffer is RGBA8, and
    // glReadPixels(GL_RGB, GL_UNSIGNED_BYTE) is not an accepted format/type
    // pair there — it raises GL_INVALID_OPERATION ("glReadPixelsRobustANGLE:
    // Invalid format and type combination") and returns nothing (JD playtest
    // report #2, 2026-10-19: the --ui-shot grab failed on the itch GL page).
    // RGBA/UNSIGNED_BYTE is the universally valid pair; read four channels
    // and drop alpha into this function's RGB contract. Desktop GL keeps the
    // direct RGB read below, byte for byte.
    {
      std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
      const size_t n = size_t(w) * size_t(h);
      for (size_t i = 0; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
      }
    }
#else
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
#endif
    // glReadPixels is bottom-up; PNG wants top-first: flip the rows in place.
    const size_t row = size_t(w) * 3;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < h / 2; ++y) {
      uint8_t* a = rgb.data() + size_t(y) * row;
      uint8_t* b = rgb.data() + size_t(h - 1 - y) * row;
      std::memcpy(tmp.data(), a, row);
      std::memcpy(a, b, row);
      std::memcpy(b, tmp.data(), row);
    }
    return true;
  }

 private:
  // Uniforms that come from the block models; re-applied after a shader reload.
  void applyMaterialUniforms() { applyMaterialUniformsTo(prog_); }
  // Same material set for another chunk program (the Phase 2 mega variant
  // shares every material uniform with the classic program).
  void applyMaterialUniformsTo(cGlProgram& p) {
    p.use();
    auto u1 = [&](const std::string& n, int v) { glUniform1i(p.uniform(n.c_str()), v); };
    auto arr = [&](const char* n, size_t i) { return std::string(n) + "[" + std::to_string(i) + "]"; };
    u1("uTex", 0); u1("uPal", 1); u1("uTile", textures_.tileSize());
    for (size_t c = 0; c < 2; ++c) { u1(arr("uWirePal", c), bindings_.stripPalette[c]); u1(arr("uRing", c), bindings_.stripRing[c]); }
    for (size_t t = 0; t < 3; ++t) {
      u1(arr("uLampPal", t), bindings_.tierPalette[t]);
      u1(arr("uLampMask", t), bindings_.tierMask[t]);
      u1(arr("uLampRim", t), bindings_.tierRim[t]);
    }
    u1("uRepMask", bindings_.pipMask); u1("uMemMask", bindings_.cellMask);
    for (size_t c = 0; c < 2; ++c) u1(arr("uDot", c), bindings_.stripDot[c]);
    u1("uDigits", bindings_.debugDigits);
    for (size_t k = 0; k < 4; ++k) u1(arr("uLeafGlow", k), bindings_.leafGlow[k]);  // 2026-10-16 crystal twinkle
    for (size_t k = 0; k < 32; ++k) u1(arr("uTintMask", k), bindings_.tintMask[k]);  // 2026-10-16 kShadeTraceTint
    for (size_t i = 0; i < animations_.size(); ++i) {
      u1(arr("uAnimStart", i), animations_[i].sequenceStart);
      u1(arr("uAnimLen", i), animations_[i].sequenceLength);
      glUniform1f(p.uniform(arr("uAnimMs", i).c_str()), animations_[i].frameMs);
    }
    for (size_t i = 0; i < animFrames_.size(); ++i) u1(arr("uAnimSeq", i), animFrames_[i]);
  }

  // vao / vbo: per-mesh path. first / count: the mesh's vertex range in the arena VBO.
  struct Gpu { GLuint vao = 0, vbo = 0; GLsizei indexCount = 0; int cx = 0, sy = 0, cz = 0; size_t first = 0, count = 0;
                 float pulseP0[4]{}; float pulseP1[4]{}; float traceP0[4]{}; float traceP1[4]{};  // wafer ground-pulse + trace blended params
                 // Phase 2 mega-draw: this mesh's rebased pattern indices in the
                 // arena index buffer (index units), and the (first, count) they
                 // were built for — rebuilt whenever the vertex range moves.
                 size_t indexFirst = 0; size_t indexBuiltFirst = ~size_t(0); GLsizei indexBuiltCount = 0;
                 // M12 canonical pull mesh (LL_GL_CANON): texel range in the canonical TBO.
                 bool canon = false; uint32_t cSlab = 0; size_t cFirst = 0, cCount = 0; GLint cFb = 0; GLsizei cVerts = 0; };
  // PBR (BinaryQuad) meshes: one VAO+VBO each (never in the arena - see uploadPbrChunkMesh).
  struct PbrGpu { GLuint vao = 0, vbo = 0; GLsizei indexCount = 0; int cx = 0, sy = 0, cz = 0; };
  void erase(int64_t key) {
    auto it = meshes_.find(key);
    if (it == meshes_.end()) return;
    if (it->second.canon) {
      MeshPlacement pl; pl.slab = it->second.cSlab; pl.first = it->second.cFirst; pl.count = it->second.cCount;
      canonUp_.release(pl);
      meshes_.erase(it);
      return;
    }
    if (useArena_) {
      freeRange(it->second.first, it->second.count);
      if (it->second.indexBuiltCount > 0) freeIndexRange(it->second.indexFirst, size_t(it->second.indexBuiltCount));
    }
    else { glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao); }
    meshes_.erase(it);
  }
  void erasePbr(int64_t key) {
    auto it = pbrMeshes_.find(key);
    if (it == pbrMeshes_.end()) return;
    glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao);
    pbrMeshes_.erase(it);
  }

  // ---- arena: one VBO (grown by doubling with glCopyBufferSubData) and one VAO; first-fit
  // free list in vertex units with coalescing. glBufferSubData into a freed range is ordered
  // by the driver after earlier draws that read it.
  // Q5a step 1 (Archie exec summary, 2026-10-28): initial arena doubled
  // (32 -> 64 MiB of verts; indices scale at x1.5 below) so a full
  // radius-16 ring fits without a mid-play grow... a grow is a
  // frame-thread realloc + full copy (the GenBuffers/DeleteBuffers
  // fingerprint in the 478.7 ms outlier second of the Build86k log).
  // The grow census logged in growArena/growIndexArena convicts the
  // exact full-ring number for the world-load pre-size that follows.
  static constexpr size_t kArenaInitialVerts = (size_t(64) << 20) / sizeof(PackedVertex);
  void bindArenaAttribs() {
    glBindVertexArray(arenaVao_);
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
    glEnableVertexAttribArray(3);  // Cortana 2026-10-15 1628: CA-03 [ENGINE] aLayerHi (byte 15)
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(15));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arenaEbo_);  // rebased arena indices (0 until the first mesh builds them)
    if (instVbo_) {  // NOT gated on megaOk_: this runs at init, before loadShaders
                     // latches megaOk_ — gating on the latch left the instanced
                     // attributes unset forever and every mega-draw read the
                     // default (0,0,0,1) attribute (first bench run, 2026-10-23).
      // Phase 2 instanced attributes (locations 4..8, divisor 1), one 19-float
      // record per visible mesh: offset(3) + pulseP0(4) + pulseP1(4) +
      // traceP0(4) + traceP1(4). The classic program does not declare these
      // locations; enabled-but-unconsumed arrays are legal, and this VAO is
      // only drawn by the classic path when megaOk_ is false (then this
      // block never ran).
      glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
      const GLsizei stride = GLsizei(19 * sizeof(float));
      glEnableVertexAttribArray(4);
      glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(0));
      glEnableVertexAttribArray(5);
      glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(12));
      glEnableVertexAttribArray(6);
      glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(28));
      glEnableVertexAttribArray(7);
      glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(44));
      glEnableVertexAttribArray(8);
      glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(60));
      for (GLuint loc = 4; loc <= 8; ++loc) glVertexAttribDivisor(loc, 1);
    }
  }
  void growArena(size_t minVerts) {
    size_t cap = std::max(arenaVerts_ ? arenaVerts_ * 2 : kArenaInitialVerts, minVerts);
    logInfo("gl33: arena grow: vertex capacity %zu -> %zu (frame-thread realloc + copy; Q5a census)", arenaVerts_, cap);
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, vbo);
    glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(cap * sizeof(PackedVertex)), nullptr, GL_STATIC_DRAW);
    if (arenaVbo_) {
      glBindBuffer(GL_COPY_READ_BUFFER, arenaVbo_);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, GLsizeiptr(arenaVerts_ * sizeof(PackedVertex)));
      glDeleteBuffers(1, &arenaVbo_);
    }
    if (!arenaVao_) glGenVertexArrays(1, &arenaVao_);
    freeRange(arenaVerts_, cap - arenaVerts_);
    arenaVbo_ = vbo;
    arenaVerts_ = cap;
    bindArenaAttribs();
  }
  std::optional<size_t> allocRange(size_t n) {
    for (auto it = arenaFree_.begin(); it != arenaFree_.end(); ++it) {
      if (it->second < n) continue;
      const size_t first = it->first, rest = it->second - n;
      arenaFree_.erase(it);
      if (rest) arenaFree_.emplace(first + n, rest);
      return first;
    }
    return std::nullopt;
  }
  void freeRange(size_t first, size_t n) {
    if (!n) return;
    auto it = arenaFree_.emplace(first, n).first;
    if (const auto next = std::next(it); next != arenaFree_.end() && it->first + it->second == next->first) {
      it->second += next->second;
      arenaFree_.erase(next);
    }
    if (it != arenaFree_.begin())
      if (const auto prev = std::prev(it); prev->first + prev->second == it->first) {
        prev->second += it->second;
        arenaFree_.erase(it);
      }
  }
  // Phase 2: the index arena mirrors the vertex arena (first-fit free list,
  // doubling growth) in index units.
  std::optional<size_t> allocIndexRange(size_t n) {
    for (auto it = arenaIndexFree_.begin(); it != arenaIndexFree_.end(); ++it) {
      if (it->second < n) continue;
      const size_t first = it->first, rest = it->second - n;
      arenaIndexFree_.erase(it);
      if (rest) arenaIndexFree_.emplace(first + n, rest);
      return first;
    }
    return std::nullopt;
  }
  void freeIndexRange(size_t first, size_t n) {
    if (!n) return;
    auto it = arenaIndexFree_.emplace(first, n).first;
    if (const auto next = std::next(it); next != arenaIndexFree_.end() && it->first + it->second == next->first) {
      it->second += next->second;
      arenaIndexFree_.erase(next);
    }
    if (it != arenaIndexFree_.begin())
      if (const auto prev = std::prev(it); prev->first + prev->second == it->first) {
        prev->second += it->second;
        arenaIndexFree_.erase(it);
      }
  }
  void growIndexArena(size_t minIndices) {
    static constexpr size_t kArenaInitialIndices = kArenaInitialVerts * 3 / 2;  // 6 indices per 4 verts
    size_t cap = std::max(arenaIndices_ ? arenaIndices_ * 2 : kArenaInitialIndices, minIndices);
    logInfo("gl33: arena grow: index capacity %zu -> %zu (frame-thread realloc + copy; Q5a census)", arenaIndices_, cap);
    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    // WebGL2 buffer-class rule (measured 2026-10-28, harness ab_rule): a
    // buffer's FIRST bind fixes its class. Staging this index buffer via
    // COPY_WRITE first made every later ELEMENT_ARRAY_BUFFER bind raise
    // INVALID_OPERATION and latch nothing... the arena VAO drew with a
    // null element binding while its payloads verified byte-correct (the
    // G3 silent void; desktop GL has no such rule, so it only died
    // through the web layer). Give the new buffer its element-class birth
    // bind under the arena VAO before the copy-target staging; COPY_WRITE
    // remains legal afterwards (Path B, same harness).
    // Create the arena VAO first when this grow is the arena's first
    // touch (G3 hardening, 2026-10-28, mirroring growArena): the birth
    // bind must never be silently skipped — a skipped bind leaves the
    // new buffer classless and the VAO drawing on a null element
    // binding, the G3 silent void itself.
    if (!arenaVao_) glGenVertexArrays(1, &arenaVao_);
    glBindVertexArray(arenaVao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arenaEbo_);
    glBindBuffer(GL_COPY_WRITE_BUFFER, ebo);
    glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(cap * sizeof(uint32_t)), nullptr, GL_STATIC_DRAW);
    if (arenaEbo_) {
      glBindBuffer(GL_COPY_READ_BUFFER, arenaEbo_);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, GLsizeiptr(arenaIndices_ * sizeof(uint32_t)));
      glDeleteBuffers(1, &arenaEbo_);
    }
    freeIndexRange(arenaIndices_, cap - arenaIndices_);
    arenaEbo_ = ebo;
    arenaIndices_ = cap;
    bindArenaAttribs();
  }
  void uploadToArena(int64_t key, const ChunkMesh& m) {
    const size_t n = m.verts.size();
    auto it = meshes_.find(key);
    if (it != meshes_.end() && it->second.count != n) { freeRange(it->second.first, it->second.count); it->second.count = 0; }
    Gpu& g = meshes_[key];
    if (g.count != n) {
      auto first = allocRange(n);
      if (!first) { growArena(n); first = allocRange(n); }
      // Fix 2026-10-11 (audit): allocation failure after the old range was freed
      // left stale first/indexCount -> draws garbage from the freed range.
      if (!first) { g.indexCount = 0; return; }
      g.first = *first;
      g.count = n;
    }
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(n / 4, kMaxQuads) * 6);
    for (int i = 0; i < 4; ++i) { g.pulseP0[i] = m.pulseP0[i]; g.pulseP1[i] = m.pulseP1[i]; g.traceP0[i] = m.traceP0[i]; g.traceP1[i] = m.traceP1[i]; }
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(g.first * sizeof(PackedVertex)), GLsizeiptr(n * sizeof(PackedVertex)), m.verts.data());
    buildMeshIndices(g);
  }
  // Phase 2: (re)build a mesh's rebased pattern indices whenever its vertex
  // range or index count moved. The pattern is procedural (the shared quad
  // strip), so index data is regenerated rather than stored. Called from
  // uploadToArena and from loadShaders (a mega latch that turns ON after
  // meshes already exist must not leave them index-less).
  void buildMeshIndices(Gpu& g) {
    if (!useArena_ || g.indexCount <= 0) return;
    if (g.indexBuiltFirst == g.first && g.indexBuiltCount == g.indexCount) return;
    if (g.indexBuiltCount > 0) { freeIndexRange(g.indexFirst, size_t(g.indexBuiltCount)); g.indexBuiltCount = 0; }
    auto firstIdx = allocIndexRange(size_t(g.indexCount));
    if (!firstIdx) { growIndexArena(size_t(g.indexCount)); firstIdx = allocIndexRange(size_t(g.indexCount)); }
    if (!firstIdx) return;
    g.indexFirst = *firstIdx;
    std::vector<uint32_t> idx(size_t(g.indexCount));
    const uint32_t quads = uint32_t(g.indexCount) / 6;
    for (uint32_t q = 0; q < quads; ++q) {
      const uint32_t b = uint32_t(g.first) + q * 4; uint32_t* o = &idx[q * 6];
      o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
    }
    glBindVertexArray(arenaVao_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, arenaEbo_);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GLintptr(g.indexFirst * sizeof(uint32_t)),
                    GLsizeiptr(idx.size() * sizeof(uint32_t)), idx.data());
    g.indexBuiltFirst = g.first;
    g.indexBuiltCount = g.indexCount;
  }
  bool useArena_ = true;
  GLuint arenaVao_ = 0, arenaVbo_ = 0;
  size_t arenaVerts_ = 0;
  std::map<size_t, size_t> arenaFree_;  // first vertex -> count
  // Phase 2 mega-draw state (2026-10-23, Elfie implementing under Archie's
  // direction): arena index buffer (rebased pattern indices per mesh), the
  // per-frame instance buffer, the LK_MEGADRAW program variant, and the
  // latched capability. megaOk_ is decided in loadShaders by a drawCount-0
  // probe of glMultiDrawElementsInstancedWEBGL; when false, every mega path
  // below is inert and the renderer is bit-identical to the pre-Phase-2 one.
  GLuint arenaEbo_ = 0, instVbo_ = 0;
  size_t instVboCapacityFloats_ = 4096 * 19;  // floats allocated in instVbo_ (Q3: grow-only, BufferSubData per frame)
  size_t arenaIndices_ = 0;
  std::map<size_t, size_t> arenaIndexFree_;  // first index -> count
  cGlProgram progMega_;
  bool megaOk_ = false;
  GLint uMegaVP_ = -1, uMegaCam_ = -1, uMegaSky_ = -1, uMegaFogEnd_ = -1, uMegaTime_ = -1,
        uMegaTwinkle_ = -1, uMegaDebug_ = -1, uMegaModelDither_ = -1, uMegaModelDitherBand_ = -1,
        uMegaGroundPulseTime_ = -1, uMegaGroundPulseOn_ = -1, uMegaGroundTraceTime_ = -1,
        uMegaGroundTraceOn_ = -1;
  std::vector<GLint> megaCounts_, megaOffsets_;
  std::vector<float> megaData_;
  std::unordered_map<int64_t, Gpu> meshes_;
  GLuint entityVao_ = 0, entityVbo_ = 0;
  GLsizei entityVertCount_ = 0;
  float entityOrigin_[3] = {0.f, 0.f, 0.f};  // GAME-H1 entity batch origin
  cGlProgram prog_, lineProg_, hudProg_;
  GLuint ebo_ = 0, lineVao_ = 0, lineVbo_ = 0;
  cGlTextureArray textures_;
  MaterialBindings bindings_;
  std::vector<ShaderAnimation> animations_;
  std::vector<int> animFrames_;
  GLint uTime_ = -1, uDebug_ = -1, uTwinkle_ = -1;
  GLint uPulseP0_ = -1, uPulseP1_ = -1, uGroundPulseTime_ = -1, uGroundPulseOn_ = -1;  // wafer ground pulse
  GLint uTraceP0_ = -1, uTraceP1_ = -1, uGroundTraceTime_ = -1, uGroundTraceOn_ = -1;  // wafer ground traces (JD 2026-10-11)
  GLint uVP_ = -1, uOffset_ = -1, uCam_ = -1, uSky_ = -1, uFogEnd_ = -1, uLineM_ = -1, uLineC_ = -1;
  GLint uModelDither_ = -1, uModelDitherBand_ = -1;  // near-camera fade (JD 2026-10-13)
  bool ditherEnabled_ = false; float ditherNear_ = 2.f, ditherFar_ = 8.f;  // setting (default on) enables it
  // PBR (BinaryQuad) path state (phase 2, Worker 3).
  cGlProgram pbrProg_;
  std::unordered_map<int64_t, PbrGpu> pbrMeshes_;
  GLuint pbrMatBuf_ = 0, pbrMatTex_ = 0;  // material table: buffer texture (GL 3.1+)
  uint64_t pbrMatGeneration_ = UINT64_MAX;  // registry generation of the uploaded table
  PbrTransparencyTier pbrTier_ = PbrTransparencyTier::ScreenDoor;  // user-selectable; game sets it
  // M12 canonical TBO uploader state (LL_GL_CANON=1).
  static constexpr GLint kCanonTexUnit = 5;
  bool canonOn_ = false;
  int canonModeSet_ = 0;
  GLuint canonVao_ = 0;
  gl33::cGlTboSlabUploader canonUp_;  // refactor step 3: IMeshUploader (owns the fix (c) slab pool)
  uint32_t canonBoundSlab_ = ~uint32_t(0);
  GLint uCanonMode_ = -1, uCanonBase_ = -1, uCanonFb_ = -1, uCanonLod_ = -1;
  static constexpr GLint kPbrMatTexUnit = 2;  // material table texture unit (0/1 are classic)
  GLint uPbrVP_ = -1, uPbrOffset_ = -1, uPbrCam_ = -1, uPbrMat_ = -1;
  GLint uPbrLightPos_ = -1, uPbrLightColor_ = -1, uPbrLightCount_ = -1, uPbrTier_ = -1;
  GLint uPbrModelDither_ = -1, uPbrModelDitherBand_ = -1;
  GLint uAmbientMode_ = -1, uSunDir_ = -1, uPbrMaterialRows_ = -1;
  AmbientMode ambientMode_ = AmbientMode::Fraction;  // JD-locked 2026-10-11; game sets it
  float frustumMarginDeg_ = 5.f;  ///< FOV culling margin, degrees (JD 2026-10-11); game sets it
  CullProtoState cullProto_;  ///< culling-cache prototypes (JD 2026-10-11); all default off
  float sunDir_[3] = {0.4f, 0.85f, 0.3f};  ///< sun direction (need not be unit)
  int pbrMatRows_ = 0;  ///< live PBR material table row count (for the OOB clamp)
  // TrueRefraction (Ultra) scene-color pass state (phase 2, Worker 4).
  GLuint sceneColorTex_ = 0;  // full-res RGBA8 copy of the opaque composite
  int sceneColorW_ = 0, sceneColorH_ = 0;
  static constexpr GLint kSceneColorTexUnit = 3;  // 0/1 classic, 2 = PBR material TBO
  GLint uSceneColor_ = -1, uSceneSize_ = -1, uSceneVP_ = -1, uRefractionPass_ = -1;
  RenderStats stats_;
  GLuint hudVao_ = 0, hudVbo_ = 0, fontTex_ = 0;
  size_t hudVboCapacityVerts_ = 0;  // verts allocated in hudVbo_ (Q3: grow-only, BufferSubData per frame)
  GLint uScreen_ = -1;
  std::vector<HudVertex> hudVerts_;
  // Sprite pass state (2026-10-12): program + streamed VBO + atlas page
  // textures; spriteAtlasUploaded_ is the registry version on the GPU
  // (UINT64_MAX = nothing uploaded yet).
  cGlProgram spriteProg_;
  GLuint spriteVao_ = 0, spriteVbo_ = 0;
  std::vector<GLuint> spriteTex_;
  uint64_t spriteAtlasUploaded_ = UINT64_MAX;
  GLint uSpriteVP_ = -1, uSpriteAtlas_ = -1, uSpriteAlphaTest_ = -1;
  std::vector<SpriteVert> spriteVerts_;
  size_t spriteVboCapacity_ = 6144;  ///< verts allocated in spriteVbo_ (grow-only, D-M5)
  // Screenshot grab state: the armed frame's size (renderFrame records it),
  // so grabFramebuffer() knows how much to read back.
  int lastFrameW_ = 0, lastFrameH_ = 0;
  bool grabArmed_ = false;
};

}  // namespace

std::unique_ptr<IRenderer> createGL33Renderer(GLProcLoader loader, bool arena) {
  if (!vcLoadGL(loader)) return nullptr;
  auto r = std::make_unique<cGL33Renderer>(arena);
  if (!r->init()) return nullptr;
  return r;
}

}  // namespace litholattice
