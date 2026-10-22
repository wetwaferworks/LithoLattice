// The render pass (colour + depth, cleared each frame), the one descriptor-set layout and
// pipeline layout every pipeline shares, and the graphics pipelines built from the embedded
// SPIR-V: chunks, depth-tested lines (block outline), overlay lines (crosshair) and the HUD.
// Private to render_vk.
#pragma once
#include <memory>
#include <string>
#include "vk_device.h"
#include "vk_handles.h"

namespace litholattice::vk {

/// Descriptor bindings of set 0 (see assets/shaders/vk/*).
enum Binding : uint32_t { kBindingFrame = 0, kBindingTextureArray = 1, kBindingPalette = 2, kBindingFont = 3,
                          kBindingSpriteAtlas = 4 };

class RenderPipelines {
 public:
  /// Construction token: only RenderPipelines's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class RenderPipelines;
    Passkey() = default;
  };
  explicit RenderPipelines(Passkey) {}
  /// Builds the render pass for the given formats and every pipeline. nullptr + `error` on failure.
  /// `packed12` (0.0.1.59 [P59-1]): chunk + chunk-blend pipelines read the 12-byte Packed12 vertex
  /// (stride 12, vk/chunk12.vert); entities always keep the 16-byte PackedVertex.
  static std::unique_ptr<RenderPipelines> create(const VulkanDevice& dev, VkFormat color, VkFormat depth,
                                                 std::string* error, bool packed12 = false);

  VkRenderPass renderPass() const { return renderPass_.get(); }
  VkDescriptorSetLayout setLayout() const { return setLayout_.get(); }
  VkPipelineLayout layout() const { return layout_.get(); }
  VkPipeline chunk() const { return chunk_.get(); }
  VkPipeline entity() const { return entity_.get(); }  ///< Cortana 2026-10-16: 16-byte PackedVertex, no culling (never Packed12)
  VkPipeline lineDepth() const { return lineDepth_.get(); }
  VkPipeline lineOverlay() const { return lineOverlay_.get(); }
  VkPipeline hud() const { return hud_.get(); }
  /// Sprite pass pipelines (JD wiring 2026-10-13): same vertex layout and
  /// shader pair; cutout = depth-write + alpha-test discard, blend /
  /// additive = depth-test only, per-shade blend factors.
  VkPipeline spriteCutout() const { return spriteCutout_.get(); }
  VkPipeline spriteBlend() const { return spriteBlend_.get(); }
  VkPipeline spriteAdditive() const { return spriteAdditive_.get(); }
  // JD Vulkan push (Cortana 2026-10-16). The main pass now has colour + depth + an HDR glow
  // target (kGlowFormat); lines and the HUD draw in the composite pass after the bloom.
  VkRenderPass compositePass() const { return compositePass_.get(); }
  VkRenderPass blurPass() const { return blurPass_.get(); }
  VkDescriptorSetLayout postSetLayout() const { return postSetLayout_.get(); }
  VkPipelineLayout postLayout() const { return postLayout_.get(); }
  VkPipeline chunkBlend() const { return chunkBlend_.get(); }   ///< sorted alpha-blend bulb pass
  VkPipeline particles() const { return particles_.get(); }    ///< procedural GPU sparks
  VkPipeline blur() const { return blur_.get(); }              ///< separable Gaussian (blurPass)
  VkPipeline composite() const { return composite_.get(); }    ///< additive bloom (compositePass)
  static constexpr VkFormat kGlowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

 private:
  UniqueRenderPass renderPass_;
  UniqueDescriptorSetLayout setLayout_;
  UniquePipelineLayout layout_;
  UniquePipeline chunk_, entity_, lineDepth_, lineOverlay_, hud_;
  UniquePipeline spriteCutout_, spriteBlend_, spriteAdditive_;
  UniqueRenderPass compositePass_, blurPass_;
  UniqueDescriptorSetLayout postSetLayout_;
  UniquePipelineLayout postLayout_;
  UniquePipeline chunkBlend_, particles_, blur_, composite_;
};

}  // namespace litholattice::vk
