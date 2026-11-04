#include <cstdlib>
#include "vk_texture_array.h"
#include "litholattice/render/texture_coverage.h"  // 2026-10-16 JD leaf pass
#include "litholattice/render/texture_dilate.h"  // Cortana 2026-10-15 1530 (CA-01)
#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>
#include "litholattice/core/log.h"

namespace litholattice::vk {
namespace {

std::optional<Buffer> stagingBuffer(const cVulkanDevice& dev, VkDeviceSize size) {
  return Buffer::create(dev, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

VkImageCreateInfo imageInfo(VkFormat format, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers,
                            VkImageUsageFlags usage) {
  auto ci = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {w, h, 1};
  ci.mipLevels = mips;
  ci.arrayLayers = layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return ci;
}

}  // namespace

std::optional<Image> uploadImage2D(const cVulkanDevice& dev, VkCommandPool pool, VkFormat format, uint32_t width,
                                   uint32_t height, const uint8_t* pixels, uint32_t bytesPerPixel) {
  const VkDeviceSize bytes = VkDeviceSize(width) * height * bytesPerPixel;
  auto staging = stagingBuffer(dev, bytes);
  auto image = Image::create(dev, imageInfo(format, width, height, 1, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
                             VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
  if (!staging || !image) return std::nullopt;
  std::memcpy(staging->mapped(), pixels, size_t(bytes));
  const bool ok = submitOnce(dev, pool, [&](VkCommandBuffer cmd) {
    transitionImage(cmd, image->handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging->handle(), image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(cmd, image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  });
  if (!ok) return std::nullopt;
  return image;
}

UniqueSampler createNearestSampler(const cVulkanDevice& dev) {
  auto si = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler s = VK_NULL_HANDLE;
  check(vkCreateSampler(dev.device(), &si, nullptr, &s), "vkCreateSampler");
  return UniqueSampler(dev.device(), s);
}

cVkTextureArray::cVkTextureArray(const cVulkanDevice& dev, VkCommandPool pool, std::function<void()> onReplaced)
    : dev_(dev), pool_(pool), onReplaced_(std::move(onReplaced)), paletteSampler_(createNearestSampler(dev)) {
  if (const char* e = std::getenv("LL_VK_ANISO")) aniso_ = float(std::atof(e));
  buildArraySampler();
}

// PROVISIONAL (JD 2026-10-24): texture-filtering stand-in (Graphics > Texture filter / Anisotropic).
// Sharp = the former fixed sampler (0.0.1.52 [C52-5]): mag NEAREST, min LINEAR + LINEAR mips.
// Bilinear = mag/min LINEAR + NEAREST mips. Trilinear = mag/min LINEAR + LINEAR mips.
// Anisotropy: requested 1..16, clamped to VkPhysicalDeviceLimits::maxSamplerAnisotropy (the device
// is created with samplerAnisotropy when offered). Small negative LOD bias (-0.25) for minification
// kept from C52-5; LL_VK_ANISO / LL_VK_LODBIAS still override for A/B shots. UNORM kept on purpose.
void cVkTextureArray::buildArraySampler() {
  auto si = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  si.magFilter = filter_ == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = filter_ == 1 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = VK_LOD_CLAMP_NONE;
  const float aniso = std::clamp(aniso_, 1.f, std::max(1.f, dev_.maxAnisotropy()));
  float bias = -0.25f;
  if (const char* e = std::getenv("LL_VK_LODBIAS")) bias = float(std::atof(e));
  bias = std::clamp(bias, -dev_.properties().limits.maxSamplerLodBias, dev_.properties().limits.maxSamplerLodBias);
  si.anisotropyEnable = aniso > 1.f ? VK_TRUE : VK_FALSE;
  si.maxAnisotropy = aniso;
  si.mipLodBias = bias;
  static const char* kNames[] = {"sharp (mag nearest, min linear + linear mips)", "bilinear (linear, nearest mips)",
                                 "trilinear (linear, linear mips)"};
  logInfo("vulkan: block texture sampler: %s, anisotropy %.0fx (device max %.0fx), LOD bias %.2f, UNORM [provisional]",
          kNames[filter_], aniso, dev_.maxAnisotropy(), bias);
  VkSampler s = VK_NULL_HANDLE;
  check(vkCreateSampler(dev_.device(), &si, nullptr, &s), "vkCreateSampler (texture array)");
  arraySampler_ = UniqueSampler(dev_.device(), s);
}

void cVkTextureArray::setFiltering(int filter, int aniso) {
  filter = std::clamp(filter, 0, 2);
  if (filter == filter_ && float(aniso) == aniso_) return;
  filter_ = filter;
  aniso_ = float(aniso);
  vkDeviceWaitIdle(dev_.device());  // the old sampler may still be in use by frames in flight
  buildArraySampler();
  if (onReplaced_ && array_) onReplaced_();  // rebind descriptors to the new sampler
}

std::optional<Image> cVkTextureArray::uploadArray(const cTextureReg& textures, uint32_t mipLevels) const {
  const uint32_t tile = uint32_t(textures.tileSize()), layers = uint32_t(textures.textures().size());
  const VkDeviceSize layerBytes = VkDeviceSize(tile) * tile * 4;
  auto staging = stagingBuffer(dev_, layerBytes * layers);
  auto image = Image::create(dev_, imageInfo(VK_FORMAT_R8G8B8A8_UNORM, tile, tile, mipLevels, layers,
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT),
                             VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT);
  if (!staging || !image) return std::nullopt;
  for (uint32_t i = 0; i < layers; ++i)
  {  // Cortana 2026-10-15 1530 (ENGINE, CA-01): bleed RGB into alpha=0 texels before the mip blits.
    std::memcpy(staging->mapped() + layerBytes * i, textures.textures()[i].image.pixels().data(), size_t(layerBytes));
    if (!dilateSkipLayer(textures.textures()[i].name))  // 2026-11-01: masks unbled (LL_MASK_NODILATE)
      dilateTransparentRGB(reinterpret_cast<uint8_t*>(staging->mapped() + layerBytes * i), int(tile), int(tile));
  }
  // 2026-10-16 (JD leaf pass): coverage-preserving CPU mips for opted-in cutout layers, copied
  // over the blitted levels after the blit chain (texture_coverage.h).
  struct CovCopy { uint32_t layer, level, size; VkDeviceSize offset; };
  std::vector<CovCopy> covCopies;
  std::vector<uint8_t> covBytes;
  for (uint32_t i = 0; i < layers; ++i) {
    if (!coverageMipLayer(textures.textures()[i].name)) continue;
    const auto levels = coverageMips(reinterpret_cast<const uint8_t*>(staging->mapped() + layerBytes * i), int(tile));
    for (size_t l = 0; l < levels.size() && l + 1 < mipLevels; ++l) {
      covCopies.push_back({i, uint32_t(l + 1), std::max(1u, tile >> (l + 1)), VkDeviceSize(covBytes.size())});
      covBytes.insert(covBytes.end(), levels[l].begin(), levels[l].end());
    }
  }
  std::optional<Buffer> covStaging;
  if (!covBytes.empty()) {
    covStaging = stagingBuffer(dev_, covBytes.size());
    if (!covStaging) return std::nullopt;
    std::memcpy(covStaging->mapped(), covBytes.data(), covBytes.size());
  }

  const VkImage img = image->handle();
  const bool ok = submitOnce(dev_, pool_, [&](VkCommandBuffer cmd) {
    transitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent = {tile, tile, 1};
    vkCmdCopyBufferToImage(cmd, staging->handle(), img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Mip chain by successive linear blits: level i-1 (now TRANSFER_SRC) -> level i.
    int32_t size = int32_t(tile);
    for (uint32_t level = 1; level < mipLevels; ++level) {
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, level - 1, 1);
      const int32_t next = std::max(1, size / 2);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, layers};
      blit.srcOffsets[1] = {size, size, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
      blit.dstOffsets[1] = {next, next, 1};
      vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
      size = next;
    }
    if (!covCopies.empty() && mipLevels > 1) {  // 2026-10-16: coverage mips over the blitted levels 1..n
      if (mipLevels > 2)
        transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, mipLevels - 2);
      std::vector<VkBufferImageCopy> regions;
      for (const CovCopy& c : covCopies) {
        VkBufferImageCopy r{};
        r.bufferOffset = c.offset;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, c.level, c.layer, 1};
        r.imageExtent = {c.size, c.size, 1};
        regions.push_back(r);
      }
      // Make the last level's blit visible before overwriting it too.
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, mipLevels - 1, 1);
      vkCmdCopyBufferToImage(cmd, covStaging->handle(), img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(regions.size()), regions.data());
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1);
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, mipLevels - 1);
      return;
    }
    // Every level but the last is TRANSFER_SRC now; the last is TRANSFER_DST.
    if (mipLevels > 1)
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, mipLevels - 1);
    transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, mipLevels - 1, 1);
  });
  if (!ok) return std::nullopt;
  return image;
}

bool cVkTextureArray::upload(const cTextureReg& textures) {
  vkDeviceWaitIdle(dev_.device());  // the old images may still be in use by frames in flight
  const uint32_t tile = uint32_t(std::max(1, textures.tileSize()));
  const uint32_t mips = uint32_t(std::bit_width(tile));  // down to 1x1, like glGenerateMipmap
  auto array = uploadArray(textures, mips);

  // Palettes: row = PaletteId, padded with transparent black to the longest palette.
  const uint32_t w = uint32_t(std::max(1, textures.paletteWidth()));
  const uint32_t h = uint32_t(std::max<size_t>(1, textures.palettes().size()));
  std::vector<uint8_t> px(size_t(w) * h * 4, 0);
  for (size_t r = 0; r < textures.palettes().size(); ++r) {
    const auto& colors = textures.palettes()[r].colors;
    for (size_t x = 0; x < colors.size(); ++x) std::memcpy(&px[(r * w + x) * 4], colors[x].data(), 4);
  }
  auto palette = uploadImage2D(dev_, pool_, VK_FORMAT_R8G8B8A8_UNORM, w, h, px.data(), 4);
  if (!array || !palette) {
    logError("vulkan: texture upload failed; keeping the previous textures");
    return false;
  }
  array_ = std::move(array);
  palette_ = std::move(palette);
  layers_ = int(textures.textures().size());
  tile_ = int(tile);
  if (onReplaced_) onReplaced_();
  return true;
}

VkDescriptorImageInfo cVkTextureArray::arrayDescriptor() const {
  return {arraySampler_.get(), array_ ? array_->view() : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkDescriptorImageInfo cVkTextureArray::paletteDescriptor() const {
  return {paletteSampler_.get(), palette_ ? palette_->view() : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

}  // namespace litholattice::vk
