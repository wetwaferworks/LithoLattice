#include "gl_texture_array.h"
#include <algorithm>
#include <string>
#include <vector>
#include "litholattice/core/log.h"
#include "litholattice/render/texture_dilate.h"  // Cortana 2026-10-15 1530 (CA-01)
#include "litholattice/render/texture_coverage.h"  // 2026-10-16 JD leaf pass

namespace litholattice::gl33 {

cGlTextureArray::~cGlTextureArray() {
  if (array_) glDeleteTextures(1, &array_);
  if (palette_) glDeleteTextures(1, &palette_);
}

bool cGlTextureArray::upload(const cTextureReg& textures) {
  if (!array_) glGenTextures(1, &array_);
  tile_ = textures.tileSize();
  layers_ = int(textures.textures().size());
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, tile_, tile_, layers_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  std::vector<std::pair<int, std::vector<std::vector<uint8_t>>>> coverage;  // 2026-10-16 JD leaf pass
  for (int i = 0; i < layers_; ++i) {
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
    dilateTransparentRGB(dilScratch_.data(), tile_, tile_);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, tile_, tile_, 1, GL_RGBA, GL_UNSIGNED_BYTE, dilScratch_.data());
    if (coverageMipLayer(textures.textures()[size_t(i)].name)) coverage.emplace_back(i, coverageMips(dilScratch_.data(), tile_));
  }
  glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
  // 2026-10-16 (JD leaf pass): opted-in cutout layers replace the driver mips with
  // coverage-preserving CPU mips (texture_coverage.h). Other layers keep the driver's.
  for (const auto& [layer, levels] : coverage)
    for (size_t l = 0; l < levels.size(); ++l) {
      const int sz = std::max(1, tile_ >> int(l + 1));
      glTexSubImage3D(GL_TEXTURE_2D_ARRAY, GLint(l + 1), 0, 0, layer, sz, sz, 1, GL_RGBA, GL_UNSIGNED_BYTE, levels[l].data());
    }
  applyFiltering();  // PROVISIONAL 2026-10-24: min/mag + anisotropy from the Graphics settings
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return glGetError() == 0;
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
    GLint n = 0;
    if (glGetIntegerv && glGetStringi) glGetIntegerv(kNumExtensions, &n);
    for (GLint i = 0; i < n && !has; ++i) {
      const char* e = reinterpret_cast<const char*>(glGetStringi(kExtensions, GLuint(i)));
      has = e && (std::string(e) == "GL_EXT_texture_filter_anisotropic" || std::string(e) == "GL_ARB_texture_filter_anisotropic");
    }
    if (has && glGetFloatv) { GLfloat m = 1.f; glGetFloatv(kMaxTexMaxAniso, &m); maxAniso_ = std::max(1.f, m); }
    logInfo("gl33: anisotropic filtering %s (max %.0fx)", has ? "available" : "not offered by the driver", maxAniso_);
  }
  return int(maxAniso_);
}

void cGlTextureArray::applyFiltering() {
  if (!array_) return;
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GLint(filter_ == 1 ? kLinearMipNearest : kLinearMipLinear));
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GLint(filter_ == 0 ? kNearest : kLinear));
  const int cap = maxAnisotropy();
  const float a = float(std::clamp(aniso_, 1, std::max(1, cap)));
  if (cap > 1 && glTexParameterf) glTexParameterf(GL_TEXTURE_2D_ARRAY, kTexMaxAniso, a);
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
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, palette_);
  glActiveTexture(GL_TEXTURE0);
}

}  // namespace litholattice::gl33
