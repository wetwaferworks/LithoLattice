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

  /// Binds array 0 to unit 0, palettes to unit 1, and a second array (if split) to unit 4.
  void bind() const;
  /// PROVISIONAL (JD 2026-10-24): 0 Sharp, 1 Bilinear, 2 Trilinear; aniso 1..16 (clamped).
  void setFiltering(int filter, int aniso);
  /// EXT/ARB_texture_filter_anisotropic cap (1 = extension missing). Needs a current context.
  int maxAnisotropy();
  /// Layers stored in each GL array when split; 0 means a single unsplit array (shader uses uTex only).
  int arrayCap() const { return arrayCap_; }

 private:
  void applyFiltering();
  static constexpr int kMaxSplitArrays = 2;
  static constexpr GLint kSecondArrayUnit = 4;  // 0/1 classic, 2 PBR TBO, 3 scene color, 5 canon
  std::vector<GLuint> arrays_;
  GLuint palette_ = 0;
  int arrayCap_ = 0;
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
