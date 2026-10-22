// CPU texture array: ITextureArray backed by host memory, for the software
// renderer. Layers are stored at the software fragment path's native tile
// size: 16x16 texels, one uint32 per texel packed 0xAARRGGBB — the exact
// layout software_shaders.h samples (texData[layer*256 + ty*16 + tx]).
#pragma once
#include <cstdint>
#include <vector>
#include "litholattice/render/texture_array.h"

namespace litholattice {

class CpuTextureArray final : public ITextureArray {
 public:
  /// The software fragment shaders sample a fixed 16x16 tile per layer,
  /// whatever resolution the source registry carries.
  static constexpr int kTileSize = 16;

  /// Replaces the contents with every layer of `textures`, box-downsampled
  /// to kTileSize x kTileSize (the same bounds/averaging biome_capture
  /// applies when it builds the software texture buffer by hand, so both
  /// feed paths agree texel-for-texel). Palettes are not stored: the
  /// software fragment inputs carry no palette data — strip/palette shading
  /// on this path resolves from the vertex mode/value fields instead.
  bool upload(const TextureRegistry& textures) override;
  int layerCount() const override { return layers_; }
  /// The stored tile resolution (kTileSize), not the source registry's.
  int tileSize() const override { return tile_; }
  /// Cortana 2026-10-16 (BUG-0003, render.tex_res): the largest tile upload() may keep
  /// (16/32/64; 0 = auto: 64, or 32 on <= 4 logical cores). LL_SW_TEXRES overrides.
  /// upload() keeps min(cap, pack size), so pack_32 stays 32 and pack_64 is full-res.
  void setMaxTile(int t) { maxTile_ = t; }
  /// Cortana 2026-10-16 (BUG-0005): box-filtered mip chain for 32/64 tiles (level 0 = data(),
  /// level k = tileSize() >> k, down to 4x4). 16x16 arrays keep one level (bit-identical path).
  static constexpr int kMaxMips = 5;
  int mipLevels() const { return levels_; }
  const uint32_t* mipData(int k) const { return k <= 0 ? data() : (k < levels_ && !mips_[k].empty() ? mips_[k].data() : data()); }

  /// Raw upload behind SoftwareRenderer::setTextures: `layers` tiles of
  /// kTileSize x kTileSize uint32 texels (0xAARRGGBB), copied. Null data or
  /// zero layers clears the array.
  void setRaw(const uint32_t* data, uint32_t layers);

  /// Stored texels for the rasterizer; nullptr when the array is empty.
  const uint32_t* data() const { return texels_.empty() ? nullptr : texels_.data(); }
  /// Ginny (Argon) 2026-10-16 Pass 2 step 4: the same texels with each 16x16 layer stored
  /// in Morton (Z) order (index = kSwMortonU[tx] | kSwMortonV[ty]; every 8x8 quadrant is
  /// one contiguous 256-byte run). Built at load by upload()/setRaw().
  const uint32_t* dataMorton() const { return texelsMorton_.empty() ? nullptr : texelsMorton_.data(); }
  uint32_t layers() const { return uint32_t(layers_); }
  /// 2026-10-16 (JD bug 1): palette rows as 0xFFRRGGBB, kPaletteWidth per row (the GL uPal
  /// texture's rows), so the software path can shade colour lamps like GL33 / Vulkan.
  static constexpr int kPaletteWidth = 65;  // 0..64: Trace ramps are 65 wide
  const uint32_t* paletteRow(int row) const {
    return row >= 0 && size_t(row + 1) * kPaletteWidth <= palettes_.size() ? &palettes_[size_t(row) * kPaletteWidth] : nullptr;
  }

 private:
  std::vector<uint32_t> texels_;  ///< layers_ * 256 texels, 0xAARRGGBB
  std::vector<uint32_t> palettes_;  ///< 2026-10-16: rows x kPaletteWidth, 0xFFRRGGBB
  std::vector<uint32_t> texelsMorton_;  ///< Pass 2 step 4: Morton-swizzled copy of texels_
  void buildMorton();
  int layers_ = 0;
  int tile_ = kTileSize;  ///< Cortana 2026-10-16: stored tile size (16/32/64)
  int maxTile_ = 0;      ///< 0 = auto
  std::vector<uint32_t> mips_[kMaxMips];  ///< Cortana 2026-10-16: mips_[k] for k >= 1
  int levels_ = 1;
  void buildMips();
};

}  // namespace litholattice
