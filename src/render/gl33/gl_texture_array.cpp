#include "gl_texture_array.h"
#include <algorithm>
#include <vector>
#include "litholattice/core/log.h"
#include "litholattice/render/texture_dilate.h"  // Cortana 2026-10-15 1530 (CA-01)
#include "litholattice/render/texture_coverage.h"  // 2026-10-16 JD leaf pass

namespace litholattice::gl33 {

cGlTextureArray::~cGlTextureArray() {
  if (array_) glDeleteTextures(1, &array_);
  if (palette_) glDeleteTextures(1, &palette_);
}

bool cGlTextureArray::upload(const TextureRegistry& textures) {
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
    // larger one a silent crop. TextureRegistry is expected to guarantee
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
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);  // crisp pixel art up close
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

void cGlTextureArray::bind() const {
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, array_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, palette_);
  glActiveTexture(GL_TEXTURE0);
}

}  // namespace litholattice::gl33
