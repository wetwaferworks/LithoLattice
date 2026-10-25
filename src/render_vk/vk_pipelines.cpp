#include "vk_pipelines.h"
#include <cstdint>
#include <span>
#include <vector>
#include "vk_frame_uniforms.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/renderer.h"
// SPIR-V generated at build time from assets/shaders/vk (glslang --vn): const uint32_t arrays.
#include "spv/chunk_frag.h"
#include "spv/chunk_vert.h"
#include "spv/chunk12_vert.h"  // 0.0.1.59 [P59-1]
#include "spv/chunk12_mega_vert.h"  // mega-draw V2
#include "spv/chunk_mega_frag.h"    // mega-draw V2
#include "spv/chunk_mega_vert.h"    // mega-draw V2
#include "spv/chunk_ssbo_frag.h"    // mega-draw V2b
#include "spv/chunk_ssbo_vert.h"
#include "spv/chunk_ssbovary_vert.h"
#include "spv/chunk12_ssbo_vert.h"
#include "spv/chunk12_ssbovary_vert.h"
#include "spv/chunk12_pull_vert.h"  // Mega-Pipeline Phase 2
#include "spv/hud_frag.h"
#include "spv/hud_vert.h"
#include "spv/line_frag.h"
#include "spv/line_vert.h"
#include "spv/particle_frag.h"
#include "spv/particle_vert.h"
#include "spv/post_frag.h"
#include "spv/post_vert.h"
#include "spv/sprite_frag.h"
#include "spv/sprite_vert.h"

namespace litholattice::vk {
namespace {

UniqueShaderModule shaderModule(const cVulkanDevice& dev, std::span<const uint32_t> code) {
  auto ci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  ci.codeSize = code.size_bytes();
  ci.pCode = code.data();
  VkShaderModule m = VK_NULL_HANDLE;
  check(vkCreateShaderModule(dev.device(), &ci, nullptr, &m), "vkCreateShaderModule");
  return UniqueShaderModule(dev.device(), m);
}

/// The fixed-function choices that differ between the pipelines.
struct PipelineDesc {
  std::span<const uint32_t> vertexCode, fragmentCode;
  uint32_t stride = 0;
  uint32_t instanceStride = 0;  ///< mega-draw V2: >0 adds binding 1 at INSTANCE rate
  std::vector<VkVertexInputAttributeDescription> attributes;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool depthTest = true, depthWrite = true, cullBack = false, alphaBlend = false;
  // Blend factors used when alphaBlend is set (defaults = the HUD's
  // SRC_ALPHA / ONE_MINUS_SRC_ALPHA). The sprite additive pipeline sets
  // dst = ONE (GL33's glBlendFunc(GL_SRC_ALPHA, GL_ONE)).
  VkBlendFactor srcBlend = VK_BLEND_FACTOR_SRC_ALPHA, dstBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  // JD Vulkan push (Cortana 2026-10-16): colour attachments of the target pass (main pass = 2:
  // frame + glow) and how attachment 1 (glow) is written: 0 masked, 1 replace, 2 additive.
  uint32_t colorCount = 2;
  int glow = 0;
};

UniquePipeline buildPipeline(const cVulkanDevice& dev, VkRenderPass pass, VkPipelineLayout layout,
                             const PipelineDesc& d) {
  const UniqueShaderModule vs = shaderModule(dev, d.vertexCode), fs = shaderModule(dev, d.fragmentCode);
  if (!vs || !fs) return {};
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs.get(), "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs.get(), "main", nullptr};

  const VkVertexInputBindingDescription bindings[2] = {{0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX},
                                                       {1, d.instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE}};
  auto vi = make<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  vi.vertexBindingDescriptionCount = d.stride ? (d.instanceStride ? 2 : 1) : 0;  // JD Vulkan push: procedural passes
  vi.pVertexBindingDescriptions = bindings;
  vi.vertexAttributeDescriptionCount = uint32_t(d.attributes.size());
  vi.pVertexAttributeDescriptions = d.attributes.data();
  auto ia = make<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  ia.topology = d.topology;
  ia.primitiveRestartEnable = VK_FALSE;  // 0.0.1.52 [A52-2] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): explicit: index 0xFFFF is a real vertex (uint16 quad indices)
  auto vp = make<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  auto rs = make<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = d.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  // The shaders negate y (OpenGL-style matrices), which keeps OpenGL's counter-clockwise front faces.
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  auto ms = make<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  auto ds = make<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
  ds.depthTestEnable = d.depthTest;
  ds.depthWriteEnable = d.depthWrite;
  ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (d.alphaBlend) {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = d.srcBlend;
    blend.dstColorBlendFactor = d.dstBlend;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  VkPipelineColorBlendAttachmentState blends[2] = {blend, VkPipelineColorBlendAttachmentState{}};
  if (d.glow > 0) {  // JD Vulkan push: HDR glow target
    blends[1].colorWriteMask = blend.colorWriteMask;
    if (d.glow == 2) {
      blends[1].blendEnable = VK_TRUE;
      blends[1].srcColorBlendFactor = blends[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      blends[1].srcAlphaBlendFactor = blends[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blends[1].colorBlendOp = blends[1].alphaBlendOp = VK_BLEND_OP_ADD;
    }
  }
  auto cb = make<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
  cb.attachmentCount = d.colorCount;
  cb.pAttachments = blends;
  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  auto dyn = make<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamic;

  auto ci = make<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dyn;
  ci.layout = layout;
  ci.renderPass = pass;
  VkPipeline p = VK_NULL_HANDLE;
  if (!check(vkCreateGraphicsPipelines(dev.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines"))
    return {};
  return UniquePipeline(dev.device(), p);
}

// JD Vulkan push (Cortana 2026-10-16): dependencies shared by every pass of the frame (main ->
// blur -> blur ... -> composite): earlier attachment writes and sampled reads are finished
// before a pass writes or samples, and its writes are visible to the next pass's sampling.
void frameDependencies(VkSubpassDependency (&dep)[2]) {
  dep[0] = VkSubpassDependency{};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_SHADER_READ_BIT;
  dep[1] = VkSubpassDependency{};
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}

// 0.0.1.52 [A52-4] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): the blur passes have no depth attachment, so
// their dependencies are colour-only: the previous pass's colour writes -> this pass's sampling, and
// the previous sampling of this target -> its colour write (WAR). No DEPTH_STENCIL stages. Not
// BY_REGION: the blur taps neighbouring texels, which is not framebuffer-local.
void blurDependencies(VkSubpassDependency (&dep)[2]) {
  dep[0] = VkSubpassDependency{};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1] = VkSubpassDependency{};
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
}

UniqueRenderPass makePass(const cVulkanDevice& dev, const VkAttachmentDescription* att, uint32_t attCount,
                          const VkAttachmentReference* colorRefs, uint32_t colorCount, const VkAttachmentReference* depthRef) {
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = colorCount;
  sub.pColorAttachments = colorRefs;
  sub.pDepthStencilAttachment = depthRef;
  VkSubpassDependency dep[2];
  if (depthRef) frameDependencies(dep);
  else blurDependencies(dep);  // [A52-4] only the blur pass has no depth
  auto ci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
  ci.attachmentCount = attCount;
  ci.pAttachments = att;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 2;
  ci.pDependencies = dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  check(vkCreateRenderPass(dev.device(), &ci, nullptr, &rp), "vkCreateRenderPass");
  return UniqueRenderPass(dev.device(), rp);
}

VkAttachmentDescription attachment(VkFormat f, VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkImageLayout from,
                                   VkImageLayout to) {
  VkAttachmentDescription a{};
  a.format = f;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = load;
  a.storeOp = store;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = from;
  a.finalLayout = to;
  return a;
}

/// Main pass: frame colour (left in COLOR_ATTACHMENT for the composite pass), depth (kept for the
/// depth-tested block outline) and the HDR glow target (sampled by the bloom).
UniqueRenderPass buildMainPass(const cVulkanDevice& dev, VkFormat color, VkFormat depth) {
  const VkAttachmentDescription att[3] = {
      attachment(color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      attachment(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL),
      attachment(RenderPipelines::kGlowFormat, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
  const VkAttachmentReference colorRefs[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                              {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
  const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  return makePass(dev, att, 3, colorRefs, 2, &depthRef);
}

/// Composite pass: loads the frame and depth, adds the bloom, draws lines + HUD, presents.
UniqueRenderPass buildCompositePass(const cVulkanDevice& dev, VkFormat color, VkFormat depth) {
  const VkAttachmentDescription att[2] = {
      attachment(color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
      attachment(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)};
  const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  return makePass(dev, att, 2, &colorRef, 1, &depthRef);
}

/// Bloom blur pass: one half-resolution glow image, fully overwritten, then sampled.
UniqueRenderPass buildBlurPass(const cVulkanDevice& dev) {
  const VkAttachmentDescription att = attachment(RenderPipelines::kGlowFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                 VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  return makePass(dev, &att, 1, &colorRef, 1, nullptr);
}

[[maybe_unused]] UniqueRenderPass buildRenderPass(const cVulkanDevice& dev, VkFormat color, VkFormat depth) {
  VkAttachmentDescription att[2]{};
  att[0].format = color;
  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  att[1] = att[0];
  att[1].format = depth;
  att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;
  // Wait for the previous user of the swapchain image / depth buffer before writing them.
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  auto ci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
  ci.attachmentCount = 2;
  ci.pAttachments = att;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 1;
  ci.pDependencies = &dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  check(vkCreateRenderPass(dev.device(), &ci, nullptr, &rp), "vkCreateRenderPass");
  return UniqueRenderPass(dev.device(), rp);
}

}  // namespace

std::unique_ptr<RenderPipelines> RenderPipelines::create(const cVulkanDevice& dev, VkFormat color, VkFormat depth,
                                                         std::string* error, bool packed12) {
  auto p = std::make_unique<RenderPipelines>(Passkey{});
  p->renderPass_ = buildMainPass(dev, color, depth);  // JD Vulkan push: + glow target
  p->compositePass_ = buildCompositePass(dev, color, depth);
  p->blurPass_ = buildBlurPass(dev);

  const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkDescriptorSetLayoutBinding bindings[] = {
      {kBindingFrame, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
      {kBindingTextureArray, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingPalette, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingFont, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingSpriteAtlas, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingMegaDraws, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},  // mega-draw V2b (unused unless LL_VK_MEGA=3)
      {kBindingArenaQuads, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxPullBlocks, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};  // Mega-Pipeline Phase 2
  auto li = make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  li.bindingCount = uint32_t(std::size(bindings));
  li.pBindings = bindings;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  if (check(vkCreateDescriptorSetLayout(dev.device(), &li, nullptr, &setLayout), "vkCreateDescriptorSetLayout"))
    p->setLayout_ = UniqueDescriptorSetLayout(dev.device(), setLayout);

  const VkPushConstantRange push{both, 0, kPushConstantBytes};
  auto pi = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  pi.setLayoutCount = 1;
  pi.pSetLayouts = &setLayout;
  pi.pushConstantRangeCount = 1;
  pi.pPushConstantRanges = &push;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (p->renderPass_ && p->setLayout_ && check(vkCreatePipelineLayout(dev.device(), &pi, nullptr, &layout), "vkCreatePipelineLayout"))
    p->layout_ = UniquePipelineLayout(dev.device(), layout);
  if (!p->layout_) {
    *error = "render pass / pipeline layout creation failed";
    return nullptr;
  }

  // Chunks: PackedVertex 12-byte attribute prefix: xy (R16G16) @0 + z (R16) @4 +
  // info (R8G8B8A8) @6 + shade (R8G8) @10. The trailing slot u32 @12 is not bound.
  // (Fix 2026-10-11: slot moved to the end so this prefix is stable.)
  PipelineDesc chunk;
  chunk.vertexCode = kSpvChunkVert;
  chunk.fragmentCode = kSpvChunkFrag;
  chunk.stride = sizeof(PackedVertex);
  chunk.attributes = {{0, 0, VK_FORMAT_R16G16_UINT, 0}, {3, 0, VK_FORMAT_R16_UINT, 4},
                      {1, 0, VK_FORMAT_R8G8B8A8_UINT, 6}, {2, 0, VK_FORMAT_R8G8_UINT, 10},
                      {4, 0, VK_FORMAT_R8_UINT, 15}};  // Cortana 2026-10-15 1628: CA-03 [ENGINE] aLayerHi
  chunk.glow = 1;  // JD Vulkan push: emissive faces feed the bloom
  // Cortana 2026-10-16: entities (billboards, Deceitor parts) = 16-byte chunk format, double-sided.
  // 0.0.1.59 [P59-1]: built from the 16-byte desc before the chunk desc may switch to Packed12
  // (entity coordinates reach ~3100 and use z's high bits, outside the Packed12 domain).
  chunk.cullBack = false;
  p->entity_ = buildPipeline(dev, p->renderPass(), layout, chunk);
  if (packed12) {
    // 0.0.1.59 [P59-1] Packed12 (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the
    // Bitmaster and the Bitmasker; layout correction and build by Cortana): stride 12; z|layerHi (R16) @4,
    // layer low byte rides in info.w; location 4 (aLayerHi) is gone.
    chunk.vertexCode = kSpvChunk12Vert;
    chunk.stride = 12;
    chunk.attributes = {{0, 0, VK_FORMAT_R16G16_UINT, 0}, {3, 0, VK_FORMAT_R16_UINT, 4},
                        {1, 0, VK_FORMAT_R8G8B8A8_UINT, 6}, {2, 0, VK_FORMAT_R8G8_UINT, 10}};
  }
  chunk.cullBack = true;
  p->chunk_ = buildPipeline(dev, p->renderPass(), layout, chunk);
  // JD Vulkan push: sorted alpha-blend pass (Glass rims 40/42): depth test, no depth write.
  {
    PipelineDesc blend = chunk;
    blend.depthWrite = false;
    // 0.0.1.58 [B58-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): back-face cull like the opaque chunk pipeline. `blend` copied `chunk` after the entity pipeline had cleared cullBack, so the blended pass drew the far faces of every Trace Bulb shell through the near ones: doubled, offset red/blue crosses (BUG-0030).
    blend.cullBack = true;
    blend.alphaBlend = true;
    blend.glow = 2;
    p->chunkBlend_ = buildPipeline(dev, p->renderPass(), layout, blend);
  }
  // Mega-draw V2 (Direction JD, design Archie, implementation Cortana): chunk + blend pipelines that read the
  // per-draw record (offset|pass, pulse P0/P1, trace P0/P1) from binding 1 at instance rate. Optional: the
  // renderer falls back to the push-constant path when they are missing.
  {
    PipelineDesc mega = chunk;
    mega.vertexCode = packed12 ? std::span<const uint32_t>(kSpvChunk12MegaVert) : std::span<const uint32_t>(kSpvChunkMegaVert);
    mega.fragmentCode = kSpvChunkMegaFrag;
    mega.instanceStride = 80;
    for (uint32_t i = 0; i < 5; ++i) mega.attributes.push_back({8 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * i});
    p->chunkMega_ = buildPipeline(dev, p->renderPass(), layout, mega);
    mega.depthWrite = false;
    mega.cullBack = true;
    mega.alphaBlend = true;
    mega.glow = 2;
    p->chunkBlendMega_ = buildPipeline(dev, p->renderPass(), layout, mega);
  }
  // Mega-draw V2b (Direction JD, design Archie, implementation Cortana): no instance binding; the shaders read the
  // record from binding 5 by gl_InstanceIndex. "Vary" = profiling variant (vertex reads, flat vec4 varyings).
  for (int vary = 0; vary < 2; ++vary) {
    PipelineDesc s = chunk;
    if (vary) {
      s.vertexCode = packed12 ? std::span<const uint32_t>(kSpvChunk12SsboVaryVert) : std::span<const uint32_t>(kSpvChunkSsboVaryVert);
      s.fragmentCode = kSpvChunkMegaFrag;
    } else {
      s.vertexCode = packed12 ? std::span<const uint32_t>(kSpvChunk12SsboVert) : std::span<const uint32_t>(kSpvChunkSsboVert);
      s.fragmentCode = kSpvChunkSsboFrag;
    }
    (vary ? p->chunkSsboVary_ : p->chunkSsbo_) = buildPipeline(dev, p->renderPass(), layout, s);
    s.depthWrite = false;
    s.cullBack = true;
    s.alphaBlend = true;
    s.glow = 2;
    (vary ? p->chunkBlendSsboVary_ : p->chunkBlendSsbo_) = buildPipeline(dev, p->renderPass(), layout, s);
  }
  // Mega-Pipeline Phase 2 (Direction JD, design Archie (Mega-Pipeline brief), implementation Cortana): vertex pulling.
  // No vertex input; the shader reads 64-bit packed quads from the arena block (binding 6). Packed12 only.
  if (packed12) {
    PipelineDesc s = chunk;
    s.vertexCode = kSpvChunk12PullVert;
    s.fragmentCode = kSpvChunkSsboFrag;
    s.stride = 0;
    s.attributes.clear();
    p->chunkPull_ = buildPipeline(dev, p->renderPass(), layout, s);
  }
  // JD Vulkan push: procedural sparks (no vertex buffer), additive into frame and glow.
  {
    PipelineDesc part;
    part.vertexCode = kSpvParticleVert;
    part.fragmentCode = kSpvParticleFrag;
    part.depthWrite = false;
    part.alphaBlend = true;
    part.srcBlend = part.dstBlend = VK_BLEND_FACTOR_ONE;
    part.glow = 2;
    p->particles_ = buildPipeline(dev, p->renderPass(), layout, part);
  }
  PipelineDesc line;
  line.vertexCode = kSpvLineVert;
  line.fragmentCode = kSpvLineFrag;
  line.stride = sizeof(Vec3);
  line.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
  line.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  line.colorCount = 1;  // JD Vulkan push: lines + HUD draw in the composite pass
  p->lineDepth_ = buildPipeline(dev, p->compositePass(), layout, line);
  line.depthTest = line.depthWrite = false;
  p->lineOverlay_ = buildPipeline(dev, p->compositePass(), layout, line);

  PipelineDesc hud;
  hud.vertexCode = kSpvHudVert;
  hud.fragmentCode = kSpvHudFrag;
  hud.stride = sizeof(HudVertex);
  hud.attributes = {{0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(HudVertex, x))},
                    {1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(HudVertex, u))},
                    {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(HudVertex, r))}};
  hud.depthTest = hud.depthWrite = false;
  hud.alphaBlend = true;
  hud.colorCount = 1;
  p->hud_ = buildPipeline(dev, p->compositePass(), layout, hud);

  // Sprites (JD wiring 2026-10-13): vk_sprite::SpriteVertex layout —
  // pos (vec3) @0, uv (vec2) @12, tint (vec4) @20, page (float) @36.
  // Double-sided like GL33 (cullBack stays false), depth tested against
  // the world; cutout writes depth, the blended passes do not.
  PipelineDesc sprite;
  sprite.vertexCode = kSpvSpriteVert;
  sprite.fragmentCode = kSpvSpriteFrag;
  sprite.stride = 40;  // sizeof(vk_sprite::SpriteVertex), asserted in vk_renderer.cpp
  sprite.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                       {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                       {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20},
                       {3, 0, VK_FORMAT_R32_SFLOAT, 36}};
  p->spriteCutout_ = buildPipeline(dev, p->renderPass(), layout, sprite);
  sprite.alphaBlend = true;
  sprite.depthWrite = false;
  p->spriteBlend_ = buildPipeline(dev, p->renderPass(), layout, sprite);
  sprite.dstBlend = VK_BLEND_FACTOR_ONE;  // additive: SRC_ALPHA, ONE (as GL33)
  p->spriteAdditive_ = buildPipeline(dev, p->renderPass(), layout, sprite);

  // JD Vulkan push: post (bloom) layout: one sampler + a 16-byte fragment push.
  {
    const VkDescriptorSetLayoutBinding pb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    auto pli = make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    pli.bindingCount = 1;
    pli.pBindings = &pb;
    VkDescriptorSetLayout psl = VK_NULL_HANDLE;
    if (check(vkCreateDescriptorSetLayout(dev.device(), &pli, nullptr, &psl), "vkCreateDescriptorSetLayout (post)"))
      p->postSetLayout_ = UniqueDescriptorSetLayout(dev.device(), psl);
    const VkPushConstantRange pp{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush)};
    auto ppi = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    ppi.setLayoutCount = 1;
    ppi.pSetLayouts = &psl;
    ppi.pushConstantRangeCount = 1;
    ppi.pPushConstantRanges = &pp;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (p->postSetLayout_ && check(vkCreatePipelineLayout(dev.device(), &ppi, nullptr, &pl), "vkCreatePipelineLayout (post)"))
      p->postLayout_ = UniquePipelineLayout(dev.device(), pl);
    PipelineDesc post;
    post.vertexCode = kSpvPostVert;
    post.fragmentCode = kSpvPostFrag;
    post.depthTest = post.depthWrite = false;
    post.colorCount = 1;
    if (p->postLayout_ && p->blurPass_ && p->compositePass_) {
      p->blur_ = buildPipeline(dev, p->blurPass(), pl, post);
      post.alphaBlend = true;
      post.srcBlend = post.dstBlend = VK_BLEND_FACTOR_ONE;
      p->composite_ = buildPipeline(dev, p->compositePass(), pl, post);
    }
  }
  if (!p->chunk_ || !p->chunkBlend_ || !p->particles_ || !p->blur_ || !p->composite_ || !p->entity_ || !p->lineDepth_ || !p->lineOverlay_ || !p->hud_ || !p->spriteCutout_ ||
      !p->spriteBlend_ || !p->spriteAdditive_) {
    *error = "graphics pipeline creation failed";
    return nullptr;
  }
  return p;
}

}  // namespace litholattice::vk
