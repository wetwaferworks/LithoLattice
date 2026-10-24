// Private to the gl33 backend: ITextureArray as a GL_TEXTURE_2D_ARRAY (block layers, mipmapped)
// plus a GL_TEXTURE_2D palette texture (one row per palette).
#pragma once
#include <vector>
#include "gl_loader.h"
#include "litholattice/render/texture_array.h"

namespace litholattice::gl33 {

class cGlTextureArray final : public ITextureArray {
 public:
  cGlTextureArray() = default;
  ~cGlTextureArray() override;
  cGlTextureArray(const cGlTextureArray&) = delete;
  cGlTextureArray& operator=(const cGlTextureArray&) = delete;

  bool upload(const cTextureReg& textures) override;
  int layerCount() const override { return layers_; }
  int tileSize() const override { return tile_; }

  /// Binds the array to texture unit 0 and the palettes to unit 1.
  void bind() const;
  /// PROVISIONAL (JD 2026-10-24): 0 Sharp, 1 Bilinear, 2 Trilinear; aniso 1..16 (clamped).
  void setFiltering(int filter, int aniso);
  /// EXT/ARB_texture_filter_anisotropic cap (1 = extension missing). Needs a current context.
  int maxAnisotropy();

 private:
  void applyFiltering();
  GLuint array_ = 0, palette_ = 0;
  int filter_ = 0, aniso_ = 8;
  float maxAniso_ = -1.f;  // -1 = not queried yet
  int layers_ = 0, tile_ = 0;
  // CREW TILE (shake 2026-10-18): reused upload scratches (were a fresh
  // tile-sized allocation per layer, plus one per palette upload). Fully
  // overwritten on every use; upload() runs on the render thread.
  std::vector<uint8_t> dilScratch_;
  std::vector<uint8_t> paletteScratch_;
};

}  // namespace litholattice::gl33
