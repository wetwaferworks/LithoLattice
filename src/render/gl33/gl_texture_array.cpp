#include "gl_texture_array.h"
#include <algorithm>
#include <string>
#include <vector>
#include "litholattice/core/log.h"
#include "litholattice/render/texture_dilate.h"  // Cortana 2026-10-15 1530 (CA-01)
#include "litholattice/render/texture_coverage.h"  // 2026-10-16 JD leaf pass

namespace litholattice::gl33 {

namespace {
// G1 instrumentation (Crew GL, 2026-10-28): GL limit tokens the loader header
// does not define. Values are the fixed GLES 3.0 / WebGL2 enum assignments.
constexpr GLenum kMaxArrayTextureLayers = 0x88FF;  // GL_MAX_ARRAY_TEXTURE_LAYERS
constexpr GLenum kMaxTextureSize = 0x0D33;         // GL_MAX_TEXTURE_SIZE

/// One-shot caps log at texture-array creation: mobile GLES3/WebGL2 drivers
/// commonly cap array layers at 256 while the pack uploads 410, which is the
/// prime suspect for the load-time "texture array upload reported a GL error".
/// Query-only; allocation behaviour is unchanged.
void logArrayCapsOnce(int tile, int layers) {
  static bool done = false;
  if (done) return;
  done = true;
  GLint maxLayers = -1, maxSize = -1;
  if (glGetIntegerv) {
    glGetIntegerv(kMaxArrayTextureLayers, &maxLayers);
    glGetIntegerv(kMaxTextureSize, &maxSize);
  }
  logInfo("gl33: texture array caps: GL_MAX_ARRAY_TEXTURE_LAYERS=%d, GL_MAX_TEXTURE_SIZE=%d; "
          "upload wants %dx%dx%d (tile x tile x layers, internalformat GL_RGBA8)",
          maxLayers, maxSize, tile, tile, layers);
  if (maxLayers >= 0 && layers > maxLayers)
    logWarn("gl33: requested layer count %d EXCEEDS GL_MAX_ARRAY_TEXTURE_LAYERS %d - "
            "upload will split across multiple texture arrays",
            layers, maxLayers);
  if (maxSize >= 0 && tile > maxSize)
    logWarn("gl33: requested tile size %d EXCEEDS GL_MAX_TEXTURE_SIZE %d", tile, maxSize);
  // Drain any error the queries themselves raised so the upload checks below
  // only report upload errors (the pre-instrumentation code also surfaced
  // just the first pending code at the end of upload()).
  if (glGetError) (void)glGetError();
}

/// G1: consume and log one pending GL error with full upload context. Returns
/// false when an error was pending. Tracking the result in upload() keeps the
/// return contract identical to the old single trailing glGetError() check:
/// any error anywhere in the upload still fails the upload.
bool checkUploadError(const char* op, int layer, int mip, int w, int h, int depth) {
  const GLenum err = glGetError ? glGetError() : GLenum(0);
  if (err == 0) return true;
  logError("gl33: texture upload GL error 0x%04X at %s (layer %d, mip %d, %dx%dx%d, internalformat GL_RGBA8)",
           unsigned(err), op, layer, mip, w, h, depth);
  return false;
}
}  // namespace

cGlTextureArray::~cGlTextureArray() {
  if (!arrays_.empty()) glDeleteTextures(int(arrays_.size()), arrays_.data());
  if (palette_) glDeleteTextures(1, &palette_);
}

namespace {
int arrayIndexForLayer(int layer, int cap) { return (cap > 0) ? layer / cap : 0; }
int localLayer(int layer, int cap) { return (cap > 0) ? layer % cap : layer; }
int arrayDepth(int index, int layers, int cap) {
  if (cap <= 0) return layers;
  const int begin = index * cap;
  return std::max(0, std::min(cap, layers - begin));
}
}  // namespace

bool cGlTextureArray::upload(const cTextureReg& textures) {
  tile_ = textures.tileSize();
  layers_ = int(textures.textures().size());
  logArrayCapsOnce(tile_, layers_);  // G1 (2026-10-28): caps vs request, once
  bool ok = true;                     // G1: any error below still fails the upload

  GLint maxLayers = 0;
  if (glGetIntegerv) glGetIntegerv(kMaxArrayTextureLayers, &maxLayers);
  if (glGetError) (void)glGetError();  // drain a failed query so it does not taint upload checks
  if (maxLayers < 1) maxLayers = layers_ > 0 ? layers_ : 1;

  arrayCap_ = 0;
  int nArr = 1;
  if (layers_ > maxLayers) {
    nArr = (layers_ + maxLayers - 1) / maxLayers;
    if (nArr > kMaxSplitArrays) {
      logError("gl33: need %d texture arrays for %d layers (cap %d), this build splits at most %d - extra layers left empty",
               nArr, layers_, maxLayers, kMaxSplitArrays);
      nArr = kMaxSplitArrays;
    }
    arrayCap_ = maxLayers;
    logInfo("gl33: splitting %d layers into %d arrays of up to %d (GL_MAX_ARRAY_TEXTURE_LAYERS=%d)",
            layers_, nArr, maxLayers, maxLayers);
  }

  if (int(arrays_.size()) != nArr) {
    if (!arrays_.empty()) glDeleteTextures(int(arrays_.size()), arrays_.data());
    arrays_.assign(size_t(nArr), 0);
    glGenTextures(nArr, arrays_.data());
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int a = 0; a < nArr; ++a) {
    const int depth = arrayDepth(a, layers_, arrayCap_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_[size_t(a)]);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, tile_, tile_, std::max(1, depth), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ok = checkUploadError("glTexImage3D array allocation", a, 0, tile_, tile_, depth) && ok;
  }

  std::vector<std::pair<int, std::vector<std::vector<uint8_t>>>> coverage;  // 2026-10-16 JD leaf pass
  for (int i = 0; i < layers_; ++i) {
    if (arrayIndexForLayer(i, arrayCap_) >= nArr) continue;

    const Image& img = textures.textures()[size_t(i)].image;
    // Tile-geometry check (P3-3, Appendix D L1): glTexSubImage3D below
    // uploads tile_ x tile_ texels from the image's buffer unconditionally.
    // A smaller image would be an out-of-bounds host read fed to GL; a
    // larger one a silent crop. cTextureReg is expected to guarantee
    // tile-sized images, but this upload is the last line of defence:
    // skip + log a mismatched layer (it stays zero-filled = black, and
    // the registry's own missing-texture path normally prevents this).
    if (img.width() != tile_ || img.height() != tile_) {
      logError("gl33: texture layer %d is %dx%d, expected %dx%d - layer left empty",
               i, img.width(), img.height(), tile_, tile_);
      continue;
    }
    // Cortana 2026-10-15 1530 (ENGINE, CA-01): bleed RGB into alpha=0 texels before glGenerateMipmap.
    // CREW TILE (shake 2026-10-18): reused scratch instead of a fresh
    // per-layer copy; assign() fully overwrites it with this layer's pixels.
    dilScratch_.assign(img.pixels().begin(), img.pixels().end());
    if (!dilateSkipLayer(textures.textures()[size_t(i)].name)) dilateTransparentRGB(dilScratch_.data(), tile_, tile_);  // 2026-11-01: masks unbled (LL_MASK_NODILATE)
    const int a = arrayIndexForLayer(i, arrayCap_);
    const int loc = localLayer(i, arrayCap_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_[size_t(a)]);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, loc, tile_, tile_, 1, GL_RGBA, GL_UNSIGNED_BYTE, dilScratch_.data());
    ok = checkUploadError("glTexSubImage3D layer upload", i, 0, tile_, tile_, 1) && ok;
    if (coverageMipLayer(textures.textures()[size_t(i)].name)) coverage.emplace_back(i, coverageMips(dilScratch_.data(), tile_));
  }
  for (int a = 0; a < nArr; ++a) {
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_[size_t(a)]);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    ok = checkUploadError("glGenerateMipmap texture array", a, -1, tile_, tile_, arrayDepth(a, layers_, arrayCap_)) && ok;
  }
  // 2026-10-16 (JD leaf pass): opted-in cutout layers replace the driver mips with
  // coverage-preserving CPU mips (texture_coverage.h). Other layers keep the driver's.
  for (const auto& [layer, levels] : coverage) {
    if (arrayIndexForLayer(layer, arrayCap_) >= nArr) continue;
    const int a = arrayIndexForLayer(layer, arrayCap_);
    const int loc = localLayer(layer, arrayCap_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_[size_t(a)]);
    for (size_t l = 0; l < levels.size(); ++l) {
      const int sz = std::max(1, tile_ >> int(l + 1));
      glTexSubImage3D(GL_TEXTURE_2D_ARRAY, GLint(l + 1), 0, 0, loc, sz, sz, 1, GL_RGBA, GL_UNSIGNED_BYTE, levels[l].data());
      ok = checkUploadError("glTexSubImage3D coverage mip upload", layer, int(l + 1), sz, sz, 1) && ok;
    }
  }
  applyFiltering();  // PROVISIONAL 2026-10-24: min/mag + anisotropy from the Graphics settings
  for (GLuint id : arrays_) {
    glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  // Palettes: row = PaletteId, padded with transparent black to the longest palette.
  const int w = textures.paletteWidth(), h = std::max<int>(1, int(textures.palettes().size()));
  paletteScratch_.assign(size_t(w) * size_t(h) * 4, 0);  // CREW TILE: reused (was per-upload)
  std::vector<uint8_t>& px = paletteScratch_;
  for (size_t r = 0; r < textures.palettes().size(); ++r) {
    const auto& colors = textures.palettes()[r].colors;
    for (size_t x = 0; x < colors.size(); ++x)
      for (size_t c = 0; c < 4; ++c) px[(r * size_t(w) + x) * 4 + c] = colors[x][c];
  }
  if (!palette_) glGenTextures(1, &palette_);
  glBindTexture(GL_TEXTURE_2D, palette_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  ok = checkUploadError("glTexImage2D palette upload", -1, 0, w, h, 1) && ok;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return ok && glGetError() == 0;
}

// PROVISIONAL (JD 2026-10-24): texture-filtering stand-in. Sharp = the former fixed setup
// (MIN LINEAR_MIPMAP_LINEAR, MAG NEAREST: crisp pixel art up close). Anisotropy via
// GL_EXT_texture_filter_anisotropic / GL_ARB_texture_filter_anisotropic when the driver offers it.
namespace {
constexpr GLenum kTexMaxAniso = 0x84FE, kMaxTexMaxAniso = 0x84FF, kNumExtensions = 0x821D, kExtensions = 0x1F03;
constexpr GLenum kLinearMipNearest = 0x2701, kLinear = 0x2601, kNearest = 0x2600, kLinearMipLinear = 0x2703;
}  // namespace

int cGlTextureArray::maxAnisotropy() {
  if (maxAniso_ < 0.f) {
    maxAniso_ = 1.f;
    bool has = false;
#ifdef __wasm__
    // Web path (G1 fix, 2026-10-28): do NOT query kNumExtensions
    // (0x821D) here. The thin layer's GetIntegerv forwards the
    // pname straight to WebGL getParameter, and NUM_EXTENSIONS
    // is not a WebGL2 getParameter pname: the call raises
    // GL_INVALID_ENUM, the sticky flag survives, and the NEXT
    // error check (the palette upload) blames the wrong call.
    // That was the true source of the "texture array upload
    // reported a GL error" 0x0500: the palette tuple was always
    // (GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE). Enumerate by
    // probing instead: the layer's GetStringi returns null past
    // the end of its supported-extensions list.
    if (glGetStringi) {
      for (GLint i = 0; i < 4096 && !has; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(kExtensions, GLuint(i)));
        if (!e) break;
        has = std::string(e) == "GL_EXT_texture_filter_anisotropic" || std::string(e) == "GL_ARB_texture_filter_anisotropic";
      }
    }
#else
    GLint n = 0;
    if (glGetIntegerv && glGetStringi) glGetIntegerv(kNumExtensions, &n);
    for (GLint i = 0; i < n && !has; ++i) {
      const char* e = reinterpret_cast<const char*>(glGetStringi(kExtensions, GLuint(i)));
      has = e && (std::string(e) == "GL_EXT_texture_filter_anisotropic" || std::string(e) == "GL_ARB_texture_filter_anisotropic");
    }
#endif
    if (has && glGetFloatv) { GLfloat m = 1.f; glGetFloatv(kMaxTexMaxAniso, &m); maxAniso_ = std::max(1.f, m); }
    logInfo("gl33: anisotropic filtering %s (max %.0fx)", has ? "available" : "not offered by the driver", maxAniso_);
  }
  return int(maxAniso_);
}

void cGlTextureArray::applyFiltering() {
  if (arrays_.empty()) return;
  const int cap = maxAnisotropy();
  const float a = float(std::clamp(aniso_, 1, std::max(1, cap)));
  for (GLuint id : arrays_) {
    glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GLint(filter_ == 1 ? kLinearMipNearest : kLinearMipLinear));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GLint(filter_ == 0 ? kNearest : kLinear));
    if (cap > 1 && glTexParameterf) glTexParameterf(GL_TEXTURE_2D_ARRAY, kTexMaxAniso, a);
  }
  logInfo("gl33: block textures: filter %s, anisotropy %.0fx [provisional]",
          filter_ == 0 ? "sharp" : filter_ == 1 ? "bilinear" : "trilinear", cap > 1 ? a : 1.f);
}

void cGlTextureArray::setFiltering(int filter, int aniso) {
  filter_ = std::clamp(filter, 0, 2);
  aniso_ = aniso;
  applyFiltering();
}

void cGlTextureArray::bind() const {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_.empty() ? 0 : arrays_[0]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, palette_);
  if (arrays_.size() > 1) {
    glActiveTexture(GL_TEXTURE0 + kSecondArrayUnit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrays_[1]);
  }
  glActiveTexture(GL_TEXTURE0);
}

}  // namespace litholattice::gl33
