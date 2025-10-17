#include "zvulkanpipelinecontext_raii.h"

#include <gflags/gflags.h>
#include "zvulkantexture.h"
#include "z3drenderervulkanbackend.h"
#include <cstdint>

namespace nim {

namespace {
constexpr vk::PipelineStageFlags2 kDefaultSrcStage = vk::PipelineStageFlagBits2::eTopOfPipe;

vk::PipelineStageFlags2 sanitizeStage(vk::PipelineStageFlags2 stage)
{
  return stage == vk::PipelineStageFlags2{} ? kDefaultSrcStage : stage;
}

vk::AccessFlags2 sanitizeAccess(vk::AccessFlags2 access)
{
  return access;
}

vk::ImageSubresourceRange makeFullSubresourceRange(vk::ImageAspectFlags aspect)
{
  auto aspectMask = aspect == vk::ImageAspectFlags{} ? vk::ImageAspectFlagBits::eColor : aspect;
  return vk::ImageSubresourceRange{aspectMask, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
}

void transitionImage(vk::raii::CommandBuffer& cmd,
                     const ZVulkanAttachmentInfo& info,
                     vk::ImageLayout newLayout,
                     vk::PipelineStageFlags2 dstStage,
                     vk::AccessFlags2 dstAccess)
{
  vk::ImageMemoryBarrier2 barrier{.srcStageMask = sanitizeStage(info.srcStage),
                                  .srcAccessMask = sanitizeAccess(info.srcAccess),
                                  .dstStageMask = sanitizeStage(dstStage),
                                  .dstAccessMask = sanitizeAccess(dstAccess),
                                  .oldLayout = info.initialLayout,
                                  .newLayout = newLayout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = info.image,
                                  .subresourceRange = makeFullSubresourceRange(info.aspect)};
  vk::DependencyInfo dependency{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  cmd.pipelineBarrier2(dependency);
  if (info.trackingTexture != nullptr) {
    info.trackingTexture->overrideCurrentLayout(newLayout);
  }
  if (auto* be = Z3DRendererVulkanBackend::current()) {
    be->notifyLayoutTransition(false);
  }
}

void transitionToFinal(vk::raii::CommandBuffer& cmd,
                       const ZVulkanAttachmentInfo& info,
                       vk::ImageLayout oldLayout,
                       vk::PipelineStageFlags2 oldStage,
                       vk::AccessFlags2 oldAccess)
{
  vk::ImageMemoryBarrier2 barrier{.srcStageMask = sanitizeStage(oldStage),
                                  .srcAccessMask = sanitizeAccess(oldAccess),
                                  .dstStageMask = sanitizeStage(info.dstStage),
                                  .dstAccessMask = sanitizeAccess(info.dstAccess),
                                  .oldLayout = oldLayout,
                                  .newLayout = info.finalLayout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = info.image,
                                  .subresourceRange = makeFullSubresourceRange(info.aspect)};
  vk::DependencyInfo dependency{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  cmd.pipelineBarrier2(dependency);
  if (info.trackingTexture != nullptr) {
    info.trackingTexture->overrideCurrentLayout(info.finalLayout);
  }
  if (auto* be = Z3DRendererVulkanBackend::current()) {
    be->notifyLayoutTransition(false);
  }
}
} // namespace

DEFINE_bool(atlas_vk_enforce_pipeline_context,
            true,
            "Enable Vulkan pipeline context debug enforcement (requires debug build)");

#ifndef NDEBUG
bool atlasVulkanPipelineContextEnforcementEnabled()
{
  return FLAGS_atlas_vk_enforce_pipeline_context;
}
#else
bool atlasVulkanPipelineContextEnforcementEnabled()
{
  return false;
}
#endif

#ifndef NDEBUG
void ZVulkanDebugStateTracker::reset(const ZVulkanGraphicsPassSpec& spec)
{
  m_expectedViewport = !spec.viewports.empty();
  m_expectedScissor = !spec.scissors.empty();
  m_expectedLineWidth = spec.lineWidth.has_value();
  m_expectedDepthBias = spec.depthBiasEnable.has_value() || spec.depthBiasConstant.has_value() ||
                        spec.depthBiasClamp.has_value() || spec.depthBiasSlope.has_value();
  m_expectedBlendConstants = spec.blendConstants.has_value();
  m_expectedStencilState =
    spec.stencilCompareMask.has_value() || spec.stencilWriteMask.has_value() || spec.stencilRef.has_value();
  m_expectedCullMode = spec.cullMode.has_value();
  m_expectedFrontFace = spec.frontFace.has_value();
  m_expectedPrimitiveRestart = spec.primitiveRestartEnable.has_value();
  m_expectedDepthTest = spec.depthTestEnable.has_value();
  m_expectedDepthWrite = spec.depthWriteEnable.has_value();
  m_expectedDepthCompare = spec.depthCompareOp.has_value();
  m_expectedStencilTest = spec.stencilTestEnable.has_value();
  m_expectedTopology = spec.topology.has_value();
  m_expectedRasterizerDiscard = spec.rasterizerDiscardEnable.has_value();
  m_expectedDescriptorSets = spec.expectedDescriptorSetCount;
  if (!m_expectedDescriptorSets && !spec.descriptorSets.empty()) {
    m_expectedDescriptorSets = static_cast<uint32_t>(spec.descriptorSets.size());
  }
  m_expectPushConstants = spec.requirePushConstants || spec.pushConstantsSize > 0;

  m_viewportKnown = false;
  m_scissorKnown = false;
  m_lineWidthKnown = false;
  m_depthBiasKnown = false;
  m_blendConstantsKnown = false;
  m_stencilStateKnown = false;
  m_cullModeKnown = false;
  m_frontFaceKnown = false;
  m_primitiveRestartKnown = false;
  m_depthTestKnown = false;
  m_depthWriteKnown = false;
  m_depthCompareKnown = false;
  m_stencilTestKnown = false;
  m_topologyKnown = false;
  m_rasterizerDiscardKnown = false;
  m_descriptorSetsBoundAny = false;
  m_maxDescriptorSetIndexBound = 0;
  m_pushConstantsKnown = false;
  m_pushConstantsSize = 0;
  m_allowQueries = spec.allowActiveQueries;
  m_allowConditional = spec.allowConditionalRendering;
}

void ZVulkanDebugStateTracker::reset(const ZVulkanComputePassSpec& spec)
{
  m_expectedViewport = false;
  m_expectedScissor = false;
  m_expectedLineWidth = false;
  m_expectedDepthBias = false;
  m_expectedBlendConstants = false;
  m_expectedStencilState = false;
  m_expectedCullMode = false;
  m_expectedFrontFace = false;
  m_expectedPrimitiveRestart = false;
  m_expectedDepthTest = false;
  m_expectedDepthWrite = false;
  m_expectedDepthCompare = false;
  m_expectedStencilTest = false;
  m_expectedTopology = false;
  m_expectedRasterizerDiscard = false;
  m_expectedDescriptorSets = spec.expectedDescriptorSetCount;
  if (!m_expectedDescriptorSets && !spec.descriptorSets.empty()) {
    m_expectedDescriptorSets = static_cast<uint32_t>(spec.descriptorSets.size());
  }
  m_expectPushConstants = spec.requirePushConstants || spec.pushConstantsSize > 0;

  m_viewportKnown = false;
  m_scissorKnown = false;
  m_lineWidthKnown = false;
  m_depthBiasKnown = false;
  m_blendConstantsKnown = false;
  m_stencilStateKnown = false;
  m_cullModeKnown = false;
  m_frontFaceKnown = false;
  m_primitiveRestartKnown = false;
  m_depthTestKnown = false;
  m_depthWriteKnown = false;
  m_depthCompareKnown = false;
  m_stencilTestKnown = false;
  m_topologyKnown = false;
  m_rasterizerDiscardKnown = false;
  m_descriptorSetsBoundAny = false;
  m_maxDescriptorSetIndexBound = 0;
  m_pushConstantsKnown = false;
  m_pushConstantsSize = 0;
  m_allowQueries = spec.allowActiveQueries;
  m_allowConditional = spec.allowConditionalRendering;
}

void ZVulkanDebugStateTracker::reset(const ZVulkanGraphicsDrawSpec& spec)
{
  m_expectedViewport = !spec.viewports.empty();
  m_expectedScissor = !spec.scissors.empty();
  m_expectedLineWidth = spec.lineWidth.has_value();
  m_expectedDepthBias = spec.depthBiasEnable.has_value() || spec.depthBiasConstant.has_value() ||
                        spec.depthBiasClamp.has_value() || spec.depthBiasSlope.has_value();
  m_expectedBlendConstants = spec.blendConstants.has_value();
  m_expectedStencilState = spec.stencilCompareMask.has_value() || spec.stencilWriteMask.has_value() ||
                           spec.stencilRef.has_value();
  m_expectedCullMode = spec.cullMode.has_value();
  m_expectedFrontFace = spec.frontFace.has_value();
  m_expectedPrimitiveRestart = spec.primitiveRestartEnable.has_value();
  m_expectedDepthTest = spec.depthTestEnable.has_value();
  m_expectedDepthWrite = spec.depthWriteEnable.has_value();
  m_expectedDepthCompare = spec.depthCompareOp.has_value();
  m_expectedStencilTest = spec.stencilTestEnable.has_value();
  m_expectedTopology = spec.topology.has_value();
  m_expectedRasterizerDiscard = spec.rasterizerDiscardEnable.has_value();
  m_expectedDescriptorSets = spec.expectedDescriptorSetCount;
  if (!m_expectedDescriptorSets && !spec.descriptorSets.empty()) {
    m_expectedDescriptorSets = static_cast<uint32_t>(spec.descriptorSets.size());
  }
  m_expectPushConstants = spec.requirePushConstants || spec.pushConstantsSize > 0;

  m_viewportKnown = false;
  m_scissorKnown = false;
  m_lineWidthKnown = false;
  m_depthBiasKnown = false;
  m_blendConstantsKnown = false;
  m_stencilStateKnown = false;
  m_cullModeKnown = false;
  m_frontFaceKnown = false;
  m_primitiveRestartKnown = false;
  m_depthTestKnown = false;
  m_depthWriteKnown = false;
  m_depthCompareKnown = false;
  m_stencilTestKnown = false;
  m_topologyKnown = false;
  m_rasterizerDiscardKnown = false;
  m_descriptorSetsBoundAny = false;
  m_maxDescriptorSetIndexBound = 0;
  m_pushConstantsKnown = false;
  m_pushConstantsSize = 0;
  m_allowQueries = spec.allowActiveQueries;
  m_allowConditional = spec.allowConditionalRendering;
}
void ZVulkanDebugStateTracker::markViewport()
{
  m_viewportKnown = true;
}

void ZVulkanDebugStateTracker::markScissor()
{
  m_scissorKnown = true;
}

void ZVulkanDebugStateTracker::markLineWidth()
{
  m_lineWidthKnown = true;
}

void ZVulkanDebugStateTracker::markDepthBias()
{
  m_depthBiasKnown = true;
}

void ZVulkanDebugStateTracker::markBlendConstants()
{
  m_blendConstantsKnown = true;
}

void ZVulkanDebugStateTracker::markStencilState()
{
  m_stencilStateKnown = true;
}

void ZVulkanDebugStateTracker::markCullMode()
{
  m_cullModeKnown = true;
}

void ZVulkanDebugStateTracker::markFrontFace()
{
  m_frontFaceKnown = true;
}

void ZVulkanDebugStateTracker::markPrimitiveRestart()
{
  m_primitiveRestartKnown = true;
}

void ZVulkanDebugStateTracker::markDepthTest()
{
  m_depthTestKnown = true;
}

void ZVulkanDebugStateTracker::markDepthWrite()
{
  m_depthWriteKnown = true;
}

void ZVulkanDebugStateTracker::markDepthCompare()
{
  m_depthCompareKnown = true;
}

void ZVulkanDebugStateTracker::markStencilTest()
{
  m_stencilTestKnown = true;
}

void ZVulkanDebugStateTracker::markTopology()
{
  m_topologyKnown = true;
}

void ZVulkanDebugStateTracker::markRasterizerDiscard()
{
  m_rasterizerDiscardKnown = true;
}

void ZVulkanDebugStateTracker::markDescriptorSets(uint32_t firstSet, uint32_t boundCount)
{
  if (boundCount == 0) {
    return;
  }
  m_descriptorSetsBoundAny = true;
  m_maxDescriptorSetIndexBound = std::max(m_maxDescriptorSetIndexBound, firstSet + boundCount);
}

void ZVulkanDebugStateTracker::markPushConstants(uint32_t size)
{
  if (size > 0) {
    m_pushConstantsKnown = true;
    m_pushConstantsSize = size;
  }
}

void ZVulkanDebugStateTracker::require(bool condition, const char* message) const
{
  if (!atlasVulkanPipelineContextEnforcementEnabled()) {
    return;
  }
  if (!condition) {
    LOG(FATAL) << message;
  }
}

void ZVulkanDebugStateTracker::assertGraphicsPreDraw(const ZVulkanGraphicsPassSpec& spec) const
{
  if (!atlasVulkanPipelineContextEnforcementEnabled()) {
    return;
  }

  require(spec.pipeline != nullptr, "Graphics pass missing pipeline");
  require(spec.pipelineLayout != nullptr, "Graphics pass missing pipeline layout");

  if (spec.activeQueries && !m_allowQueries) {
    LOG(FATAL) << "Graphics pass uses active queries without permission";
  }
  if (spec.conditionalRendering && !m_allowConditional) {
    LOG(FATAL) << "Graphics pass uses conditional rendering without permission";
  }

  if (m_expectedViewport) {
    require(m_viewportKnown, "Viewport must be set for this graphics pass");
  }
  if (m_expectedScissor) {
    require(m_scissorKnown, "Scissor must be set for this graphics pass");
  }
  if (m_expectedLineWidth) {
    require(m_lineWidthKnown, "Line width must be set for this graphics pass");
  }
  if (m_expectedDepthBias) {
    require(m_depthBiasKnown, "Depth bias must be configured for this graphics pass");
  }
  if (m_expectedBlendConstants) {
    require(m_blendConstantsKnown, "Blend constants must be set for this graphics pass");
  }
  if (m_expectedStencilState) {
    require(m_stencilStateKnown, "Stencil masks/refs must be configured for this graphics pass");
  }
  if (m_expectedCullMode) {
    require(m_cullModeKnown, "Cull mode must be specified for this graphics pass");
  }
  if (m_expectedFrontFace) {
    require(m_frontFaceKnown, "Front face must be specified for this graphics pass");
  }
  if (m_expectedPrimitiveRestart) {
    require(m_primitiveRestartKnown, "Primitive restart enable must be specified");
  }
  if (m_expectedDepthTest) {
    require(m_depthTestKnown, "Depth test enable must be specified");
  }
  if (m_expectedDepthWrite) {
    require(m_depthWriteKnown, "Depth write enable must be specified");
  }
  if (m_expectedDepthCompare) {
    require(m_depthCompareKnown, "Depth compare op must be specified");
  }
  if (m_expectedStencilTest) {
    require(m_stencilTestKnown, "Stencil test enable must be specified");
  }
  if (m_expectedTopology) {
    require(m_topologyKnown, "Primitive topology must be specified");
  }
  if (m_expectedRasterizerDiscard) {
    require(m_rasterizerDiscardKnown, "Rasterizer discard state must be specified");
  }

  if (m_expectedDescriptorSets) {
    require(m_descriptorSetsBoundAny, "Descriptor sets were not bound");
    require(m_maxDescriptorSetIndexBound >= *m_expectedDescriptorSets, "Descriptor set coverage incomplete");
  }

  if (m_expectPushConstants) {
    require(m_pushConstantsKnown, "Push constants required by layout but not provided");
    require(m_pushConstantsSize >= spec.pushConstantsSize, "Push constant size smaller than required");
  }
}

void ZVulkanDebugStateTracker::assertGraphicsPreDraw(const ZVulkanGraphicsDrawSpec& spec) const
{
  if (!atlasVulkanPipelineContextEnforcementEnabled()) {
    return;
  }

  require((spec.pipeline != nullptr) || static_cast<VkPipeline>(spec.pipelineHandle) != VK_NULL_HANDLE,
          "Graphics draw missing pipeline");
  require((spec.pipelineLayout != nullptr) ||
            static_cast<VkPipelineLayout>(spec.pipelineLayoutHandle) != VK_NULL_HANDLE,
          "Graphics draw missing pipeline layout");

  if (spec.activeQueries && !m_allowQueries) {
    LOG(FATAL) << "Graphics draw uses active queries without permission";
  }
  if (spec.conditionalRendering && !m_allowConditional) {
    LOG(FATAL) << "Graphics draw uses conditional rendering without permission";
  }

  if (m_expectedViewport) {
    require(m_viewportKnown, "Viewport must be set for this graphics draw");
  }
  if (m_expectedScissor) {
    require(m_scissorKnown, "Scissor must be set for this graphics draw");
  }
  if (m_expectedLineWidth) {
    require(m_lineWidthKnown, "Line width must be set for this graphics draw");
  }
  if (m_expectedDepthBias) {
    require(m_depthBiasKnown, "Depth bias must be configured for this graphics draw");
  }
  if (m_expectedBlendConstants) {
    require(m_blendConstantsKnown, "Blend constants must be set for this graphics draw");
  }
  if (m_expectedStencilState) {
    require(m_stencilStateKnown, "Stencil masks/refs must be configured for this graphics draw");
  }
  if (m_expectedCullMode) {
    require(m_cullModeKnown, "Cull mode must be specified for this graphics draw");
  }
  if (m_expectedFrontFace) {
    require(m_frontFaceKnown, "Front face must be specified for this graphics draw");
  }
  if (m_expectedPrimitiveRestart) {
    require(m_primitiveRestartKnown, "Primitive restart enable must be specified");
  }
  if (m_expectedDepthTest) {
    require(m_depthTestKnown, "Depth test enable must be specified");
  }
  if (m_expectedDepthWrite) {
    require(m_depthWriteKnown, "Depth write enable must be specified");
  }
  if (m_expectedDepthCompare) {
    require(m_depthCompareKnown, "Depth compare op must be specified");
  }
  if (m_expectedStencilTest) {
    require(m_stencilTestKnown, "Stencil test enable must be specified");
  }
  if (m_expectedTopology) {
    require(m_topologyKnown, "Primitive topology must be specified");
  }
  if (m_expectedRasterizerDiscard) {
    require(m_rasterizerDiscardKnown, "Rasterizer discard state must be specified");
  }

  if (m_expectedDescriptorSets) {
    require(m_descriptorSetsBoundAny, "Descriptor sets were not bound");
    require(m_maxDescriptorSetIndexBound >= *m_expectedDescriptorSets, "Descriptor set coverage incomplete");
  }

  if (m_expectPushConstants) {
    require(m_pushConstantsKnown, "Push constants required by layout but not provided");
    require(m_pushConstantsSize >= spec.pushConstantsSize, "Push constant size smaller than required");
  }
}
void ZVulkanDebugStateTracker::assertComputePreDispatch(const ZVulkanComputePassSpec& spec) const
{
  if (!atlasVulkanPipelineContextEnforcementEnabled()) {
    return;
  }

  require(spec.pipeline != nullptr, "Compute pass missing pipeline");
  require(spec.pipelineLayout != nullptr, "Compute pass missing pipeline layout");

  if (spec.activeQueries && !m_allowQueries) {
    LOG(FATAL) << "Compute pass uses active queries without permission";
  }
  if (spec.conditionalRendering && !m_allowConditional) {
    LOG(FATAL) << "Compute pass uses conditional rendering without permission";
  }

  if (m_expectedDescriptorSets) {
    require(m_descriptorSetsBoundAny, "Descriptor sets were not bound for compute pass");
    require(m_maxDescriptorSetIndexBound >= *m_expectedDescriptorSets,
            "Descriptor set coverage incomplete for compute pass");
  }

  if (m_expectPushConstants) {
    require(m_pushConstantsKnown, "Compute pass requires push constants but none were provided");
  }
}
#endif // NDEBUG

ZVulkanPipelineCommandRecorder::ZVulkanPipelineCommandRecorder(vk::raii::CommandBuffer& commandBuffer)
  : m_commandBuffer(commandBuffer)
{}

vk::ImageLayout ZVulkanPipelineCommandRecorder::depthAttachmentLayoutForAspect(vk::ImageAspectFlags aspect)
{
  if (aspect & vk::ImageAspectFlagBits::eStencil) {
    return vk::ImageLayout::eDepthStencilAttachmentOptimal;
  }
  return vk::ImageLayout::eDepthAttachmentOptimal;
}

void ZVulkanPipelineCommandRecorder::validateAttachmentInfo(const ZVulkanAttachmentInfo& info, const char* role)
{
  CHECK(static_cast<VkImage>(info.image) != VK_NULL_HANDLE) << role << " attachment missing image handle";
  CHECK(static_cast<VkImageView>(info.view) != VK_NULL_HANDLE) << role << " attachment missing image view";
  CHECK(info.format != vk::Format::eUndefined) << role << " attachment missing format";
  if (info.loadOp == vk::AttachmentLoadOp::eLoad) {
    CHECK(info.initialLayout != vk::ImageLayout::eUndefined)
      << role << " attachment loadOp=LOAD requires a valid initialLayout";
  }
  CHECK(info.finalLayout != vk::ImageLayout::eUndefined) << role << " attachment finalLayout must be specified";
}

void ZVulkanPipelineCommandRecorder::validateDescriptorSets(const ZVulkanGraphicsPassSpec& spec)
{
  const uint32_t defaultExpected = spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size());
  const uint32_t expected = spec.expectedDescriptorSetCount ? *spec.expectedDescriptorSetCount : defaultExpected;
  if (expected == 0) {
    return;
  }
  uint32_t boundMax = 0;
  if (!spec.descriptorSets.empty()) {
    boundMax = std::max(boundMax, spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size()));
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (!bind.sets.empty()) {
      boundMax = std::max(boundMax, bind.firstSet + static_cast<uint32_t>(bind.sets.size()));
    }
  }
  CHECK(boundMax >= expected) << "Graphics pass has insufficient descriptor set coverage";
}

void ZVulkanPipelineCommandRecorder::validateDescriptorSets(const ZVulkanComputePassSpec& spec)
{
  const uint32_t defaultExpected = spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size());
  const uint32_t expected = spec.expectedDescriptorSetCount ? *spec.expectedDescriptorSetCount : defaultExpected;
  if (expected == 0) {
    return;
  }
  uint32_t boundMax = 0;
  if (!spec.descriptorSets.empty()) {
    boundMax = std::max(boundMax, spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size()));
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (!bind.sets.empty()) {
      boundMax = std::max(boundMax, bind.firstSet + static_cast<uint32_t>(bind.sets.size()));
    }
  }
  CHECK(boundMax >= expected) << "Compute pass has insufficient descriptor set coverage";
}

void ZVulkanPipelineCommandRecorder::validateDescriptorSets(const ZVulkanGraphicsDrawSpec& spec)
{
  const uint32_t defaultExpected = spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size());
  const uint32_t expected = spec.expectedDescriptorSetCount ? *spec.expectedDescriptorSetCount : defaultExpected;
  if (expected == 0) {
    return;
  }
  uint32_t boundMax = 0;
  if (!spec.descriptorSets.empty()) {
    boundMax = std::max(boundMax, spec.descriptorSetFirst + static_cast<uint32_t>(spec.descriptorSets.size()));
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (!bind.sets.empty()) {
      boundMax = std::max(boundMax, bind.firstSet + static_cast<uint32_t>(bind.sets.size()));
    }
  }
  CHECK(boundMax >= expected) << "Graphics draw has insufficient descriptor set coverage";
}

void ZVulkanPipelineCommandRecorder::recordGraphicsPass(const ZVulkanGraphicsPassSpec& spec,
                                                        const std::function<void(vk::raii::CommandBuffer&)>& drawFn)
{
  const vk::Pipeline pipelineHandle =
    spec.pipelineHandle ? spec.pipelineHandle : (spec.pipeline ? **spec.pipeline : vk::Pipeline{});
  const vk::PipelineLayout layoutHandle = spec.pipelineLayoutHandle
                                            ? spec.pipelineLayoutHandle
                                            : (spec.pipelineLayout ? **spec.pipelineLayout : vk::PipelineLayout{});

  CHECK(static_cast<VkPipeline>(pipelineHandle) != VK_NULL_HANDLE) << "Graphics pass requires a pipeline";
  CHECK(static_cast<VkPipelineLayout>(layoutHandle) != VK_NULL_HANDLE) << "Graphics pass requires a pipeline layout";
  CHECK(!spec.colorAttachments.empty() || spec.depthStencilAttachment.has_value())
    << "Graphics pass requires at least one attachment";

#ifndef NDEBUG
  m_debug.reset(spec);
#endif

  constexpr vk::PipelineStageFlags2 kRenderColorStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  constexpr vk::AccessFlags2 kRenderColorAccess =
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;

  for (const auto& attachment : spec.colorAttachments) {
    validateAttachmentInfo(attachment, "Color");
    CHECK(attachment.loadOp != vk::AttachmentLoadOp::eNone) << "Dynamic rendering requires explicit loadOp";
    transitionImage(m_commandBuffer,
                    attachment,
                    vk::ImageLayout::eColorAttachmentOptimal,
                    kRenderColorStage,
                    kRenderColorAccess);
  }
  constexpr vk::PipelineStageFlags2 kRenderDepthStage =
    vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eEarlyFragmentTests;
  constexpr vk::AccessFlags2 kRenderDepthAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
  if (spec.depthStencilAttachment) {
    validateAttachmentInfo(*spec.depthStencilAttachment, "Depth/stencil");
    transitionImage(m_commandBuffer,
                    *spec.depthStencilAttachment,
                    depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect),
                    kRenderDepthStage,
                    kRenderDepthAccess);
  }

  std::vector<vk::RenderingAttachmentInfo> colorInfos;
  colorInfos.reserve(spec.colorAttachments.size());
  for (const auto& attachment : spec.colorAttachments) {
    colorInfos.emplace_back(vk::RenderingAttachmentInfo{.imageView = attachment.view,
                                                        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                        .loadOp = attachment.loadOp,
                                                        .storeOp = attachment.storeOp,
                                                        .clearValue = attachment.clearValue});
  }

  std::optional<vk::RenderingAttachmentInfo> depthAttachmentInfo{};
  std::optional<vk::RenderingAttachmentInfo> stencilAttachmentInfo{};
  if (spec.depthStencilAttachment) {
    const auto layout = depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect);
    depthAttachmentInfo = vk::RenderingAttachmentInfo{.imageView = spec.depthStencilAttachment->view,
                                                      .imageLayout = layout,
                                                      .loadOp = spec.depthStencilAttachment->loadOp,
                                                      .storeOp = spec.depthStencilAttachment->storeOp,
                                                      .clearValue = spec.depthStencilAttachment->clearValue};
    if (spec.depthStencilAttachment->aspect & vk::ImageAspectFlagBits::eStencil) {
      stencilAttachmentInfo = depthAttachmentInfo;
    }
  }

  vk::RenderingInfo renderingInfo{.renderArea = spec.renderArea,
                                  .layerCount = 1,
                                  .colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
                                  .pColorAttachments = colorInfos.data(),
                                  .pDepthAttachment = depthAttachmentInfo ? &*depthAttachmentInfo : nullptr,
                                  .pStencilAttachment = stencilAttachmentInfo ? &*stencilAttachmentInfo : nullptr};

  m_commandBuffer.beginRendering(renderingInfo);

  m_commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelineHandle);

  validateDescriptorSets(spec);
  if (!spec.descriptorSets.empty()) {
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       layoutHandle,
                                       spec.descriptorSetFirst,
                                       spec.descriptorSets,
                                       spec.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(spec.descriptorSetFirst, static_cast<uint32_t>(spec.descriptorSets.size()));
#endif
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (bind.sets.empty()) {
      continue;
    }
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       layoutHandle,
                                       bind.firstSet,
                                       bind.sets,
                                       bind.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(bind.firstSet, static_cast<uint32_t>(bind.sets.size()));
#endif
  }

  if (spec.pushConstantsSize > 0) {
    CHECK(spec.pushConstantsData != nullptr) << "Graphics pass push constants require data";
    const auto* bytes = static_cast<const std::uint8_t*>(spec.pushConstantsData);
    vk::ArrayProxy<const std::uint8_t> payload(spec.pushConstantsSize, bytes);
    m_commandBuffer.pushConstants(layoutHandle, spec.pushConstantsStages, 0, payload);
#ifndef NDEBUG
    m_debug.markPushConstants(spec.pushConstantsSize);
#endif
  }

  if (!spec.vertexBuffers.empty()) {
    CHECK(spec.vertexBuffers.size() == spec.vertexOffsets.size())
      << "Vertex buffers and offsets must have matching counts";
    m_commandBuffer.bindVertexBuffers(0, spec.vertexBuffers, spec.vertexOffsets);
  }
  if (spec.indexCount > 0) {
    CHECK(static_cast<VkBuffer>(spec.indexBuffer) != VK_NULL_HANDLE) << "Indexed draw missing index buffer";
    m_commandBuffer.bindIndexBuffer(spec.indexBuffer, spec.indexOffset, spec.indexType);
  }

  if (!spec.viewports.empty()) {
    m_commandBuffer.setViewport(0, spec.viewports);
#ifndef NDEBUG
    m_debug.markViewport();
#endif
  }
  if (!spec.scissors.empty()) {
    m_commandBuffer.setScissor(0, spec.scissors);
#ifndef NDEBUG
    m_debug.markScissor();
#endif
  }
  if (spec.lineWidth) {
    m_commandBuffer.setLineWidth(*spec.lineWidth);
#ifndef NDEBUG
    m_debug.markLineWidth();
#endif
  }
  if (spec.depthBiasEnable) {
    m_commandBuffer.setDepthBiasEnableEXT(*spec.depthBiasEnable);
#ifndef NDEBUG
    m_debug.markDepthBias();
#endif
  }
  if (spec.depthBiasConstant || spec.depthBiasClamp || spec.depthBiasSlope) {
    m_commandBuffer.setDepthBias(spec.depthBiasConstant.value_or(0.0f),
                                 spec.depthBiasClamp.value_or(0.0f),
                                 spec.depthBiasSlope.value_or(0.0f));
#ifndef NDEBUG
    m_debug.markDepthBias();
#endif
  }
  if (spec.blendConstants) {
    m_commandBuffer.setBlendConstants(spec.blendConstants->data());
#ifndef NDEBUG
    m_debug.markBlendConstants();
#endif
  }
  if (spec.stencilCompareMask) {
    m_commandBuffer.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilCompareMask);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.stencilWriteMask) {
    m_commandBuffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilWriteMask);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.stencilRef) {
    m_commandBuffer.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilRef);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.cullMode) {
    m_commandBuffer.setCullMode(*spec.cullMode);
#ifndef NDEBUG
    m_debug.markCullMode();
#endif
  }
  if (spec.frontFace) {
    m_commandBuffer.setFrontFace(*spec.frontFace);
#ifndef NDEBUG
    m_debug.markFrontFace();
#endif
  }
  if (spec.primitiveRestartEnable) {
    m_commandBuffer.setPrimitiveRestartEnable(*spec.primitiveRestartEnable);
#ifndef NDEBUG
    m_debug.markPrimitiveRestart();
#endif
  }
  if (spec.depthTestEnable) {
    m_commandBuffer.setDepthTestEnable(*spec.depthTestEnable);
#ifndef NDEBUG
    m_debug.markDepthTest();
#endif
  }
  if (spec.depthWriteEnable) {
    m_commandBuffer.setDepthWriteEnable(*spec.depthWriteEnable);
#ifndef NDEBUG
    m_debug.markDepthWrite();
#endif
  }
  if (spec.depthCompareOp) {
    m_commandBuffer.setDepthCompareOp(*spec.depthCompareOp);
#ifndef NDEBUG
    m_debug.markDepthCompare();
#endif
  }
  if (spec.stencilTestEnable) {
    m_commandBuffer.setStencilTestEnable(*spec.stencilTestEnable);
#ifndef NDEBUG
    m_debug.markStencilTest();
#endif
  }
  if (spec.topology) {
    m_commandBuffer.setPrimitiveTopologyEXT(*spec.topology);
#ifndef NDEBUG
    m_debug.markTopology();
#endif
  }
  if (spec.rasterizerDiscardEnable) {
    m_commandBuffer.setRasterizerDiscardEnable(*spec.rasterizerDiscardEnable);
#ifndef NDEBUG
    m_debug.markRasterizerDiscard();
#endif
  }

#ifndef NDEBUG
  m_debug.assertGraphicsPreDraw(spec);
#endif

  if (drawFn) {
    drawFn(m_commandBuffer);
  } else {
    if (spec.indexCount > 0) {
      m_commandBuffer.drawIndexed(spec.indexCount,
                                  spec.instanceCount,
                                  spec.firstIndex,
                                  spec.vertexOffset,
                                  spec.firstInstance);
    } else {
      CHECK(spec.vertexCount > 0) << "Graphics pass requires vertexCount when not indexed";
      m_commandBuffer.draw(spec.vertexCount, spec.instanceCount, spec.firstVertex, spec.firstInstance);
    }
  }

  m_commandBuffer.endRendering();

  for (const auto& attachment : spec.colorAttachments) {
    transitionToFinal(m_commandBuffer,
                      attachment,
                      vk::ImageLayout::eColorAttachmentOptimal,
                      kRenderColorStage,
                      kRenderColorAccess);
  }
  if (spec.depthStencilAttachment) {
    transitionToFinal(m_commandBuffer,
                      *spec.depthStencilAttachment,
                      depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect),
                      kRenderDepthStage,
                      kRenderDepthAccess);
  }
}

void ZVulkanPipelineCommandRecorder::recordComputePass(const ZVulkanComputePassSpec& spec)
{
  CHECK(spec.pipeline != nullptr) << "Compute pass requires a pipeline";
  CHECK(spec.pipelineLayout != nullptr) << "Compute pass requires a pipeline layout";

#ifndef NDEBUG
  m_debug.reset(spec);
#endif

  m_commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, **spec.pipeline);

  validateDescriptorSets(spec);
  if (!spec.descriptorSets.empty()) {
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                       **spec.pipelineLayout,
                                       spec.descriptorSetFirst,
                                       spec.descriptorSets,
                                       spec.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(spec.descriptorSetFirst, static_cast<uint32_t>(spec.descriptorSets.size()));
#endif
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (bind.sets.empty()) {
      continue;
    }
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                       **spec.pipelineLayout,
                                       bind.firstSet,
                                       bind.sets,
                                       bind.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(bind.firstSet, static_cast<uint32_t>(bind.sets.size()));
#endif
  }

  if (spec.pushConstantsSize > 0) {
    CHECK(spec.pushConstantsData != nullptr) << "Compute pass push constants require data";
    const auto* bytes = static_cast<const std::uint8_t*>(spec.pushConstantsData);
    vk::ArrayProxy<const std::uint8_t> payload(spec.pushConstantsSize, bytes);
    m_commandBuffer.pushConstants(**spec.pipelineLayout, spec.pushConstantsStages, 0, payload);
#ifndef NDEBUG
    m_debug.markPushConstants(spec.pushConstantsSize);
#endif
  }

#ifndef NDEBUG
  m_debug.assertComputePreDispatch(spec);
#endif

  CHECK(spec.groupX > 0 && spec.groupY > 0 && spec.groupZ > 0) << "Compute dispatch groups must be non-zero";
  m_commandBuffer.dispatch(spec.groupX, spec.groupY, spec.groupZ);
}

void ZVulkanPipelineCommandRecorder::beginRenderingSegment(const ZVulkanRenderingSegmentSpec& spec)
{
  // Pre transitions
  constexpr vk::PipelineStageFlags2 kRenderColorStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  constexpr vk::AccessFlags2 kRenderColorAccess =
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
  for (const auto& attachment : spec.colorAttachments) {
    validateAttachmentInfo(attachment, "Color");
    CHECK(attachment.loadOp != vk::AttachmentLoadOp::eNone) << "Dynamic rendering requires explicit loadOp";
    transitionImage(m_commandBuffer,
                    attachment,
                    vk::ImageLayout::eColorAttachmentOptimal,
                    kRenderColorStage,
                    kRenderColorAccess);
  }
  constexpr vk::PipelineStageFlags2 kRenderDepthStage =
    vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eEarlyFragmentTests;
  constexpr vk::AccessFlags2 kRenderDepthAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
  if (spec.depthStencilAttachment) {
    validateAttachmentInfo(*spec.depthStencilAttachment, "Depth/stencil");
    transitionImage(m_commandBuffer,
                    *spec.depthStencilAttachment,
                    depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect),
                    kRenderDepthStage,
                    kRenderDepthAccess);
  }

  // Build rendering info and begin
  std::vector<vk::RenderingAttachmentInfo> colorInfos;
  colorInfos.reserve(spec.colorAttachments.size());
  for (const auto& attachment : spec.colorAttachments) {
    colorInfos.emplace_back(vk::RenderingAttachmentInfo{.imageView = attachment.view,
                                                        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                        .loadOp = attachment.loadOp,
                                                        .storeOp = attachment.storeOp,
                                                        .clearValue = attachment.clearValue});
  }
  std::optional<vk::RenderingAttachmentInfo> depthAttachmentInfo{};
  std::optional<vk::RenderingAttachmentInfo> stencilAttachmentInfo{};
  if (spec.depthStencilAttachment) {
    const auto layout = depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect);
    depthAttachmentInfo = vk::RenderingAttachmentInfo{.imageView = spec.depthStencilAttachment->view,
                                                      .imageLayout = layout,
                                                      .loadOp = spec.depthStencilAttachment->loadOp,
                                                      .storeOp = spec.depthStencilAttachment->storeOp,
                                                      .clearValue = spec.depthStencilAttachment->clearValue};
    if (spec.depthStencilAttachment->aspect & vk::ImageAspectFlagBits::eStencil) {
      stencilAttachmentInfo = depthAttachmentInfo;
    }
  }
  vk::RenderingInfo renderingInfo{.renderArea = spec.renderArea,
                                  .layerCount = 1,
                                  .colorAttachmentCount = static_cast<uint32_t>(colorInfos.size()),
                                  .pColorAttachments = colorInfos.data(),
                                  .pDepthAttachment = depthAttachmentInfo ? &*depthAttachmentInfo : nullptr,
                                  .pStencilAttachment = stencilAttachmentInfo ? &*stencilAttachmentInfo : nullptr};
  m_commandBuffer.beginRendering(renderingInfo);
  m_segmentActive = true;
}

void ZVulkanPipelineCommandRecorder::endRenderingSegment(const ZVulkanRenderingSegmentSpec& spec)
{
  m_commandBuffer.endRendering();
  m_segmentActive = false;
  // Post transitions
  constexpr vk::PipelineStageFlags2 kRenderColorStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  constexpr vk::AccessFlags2 kRenderColorAccess =
    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
  for (const auto& attachment : spec.colorAttachments) {
    transitionToFinal(m_commandBuffer,
                      attachment,
                      vk::ImageLayout::eColorAttachmentOptimal,
                      kRenderColorStage,
                      kRenderColorAccess);
  }
  constexpr vk::PipelineStageFlags2 kRenderDepthStage =
    vk::PipelineStageFlagBits2::eLateFragmentTests | vk::PipelineStageFlagBits2::eEarlyFragmentTests;
  constexpr vk::AccessFlags2 kRenderDepthAccess =
    vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
  if (spec.depthStencilAttachment) {
    transitionToFinal(m_commandBuffer,
                      *spec.depthStencilAttachment,
                      depthAttachmentLayoutForAspect(spec.depthStencilAttachment->aspect),
                      kRenderDepthStage,
                      kRenderDepthAccess);
  }
}

void ZVulkanPipelineCommandRecorder::recordGraphicsDraw(const ZVulkanGraphicsDrawSpec& spec,
                                                        const std::function<void(vk::raii::CommandBuffer&)>& drawFn)
{
  const vk::Pipeline pipelineHandle =
    spec.pipelineHandle ? spec.pipelineHandle : (spec.pipeline ? **spec.pipeline : vk::Pipeline{});
  const vk::PipelineLayout layoutHandle = spec.pipelineLayoutHandle
                                            ? spec.pipelineLayoutHandle
                                            : (spec.pipelineLayout ? **spec.pipelineLayout : vk::PipelineLayout{});

  CHECK(static_cast<VkPipeline>(pipelineHandle) != VK_NULL_HANDLE) << "Graphics draw requires a pipeline";
  CHECK(static_cast<VkPipelineLayout>(layoutHandle) != VK_NULL_HANDLE) << "Graphics draw requires a pipeline layout";

#ifndef NDEBUG
  m_debug.reset(spec);
#endif

  m_commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelineHandle);

  validateDescriptorSets(spec);
  if (!spec.descriptorSets.empty()) {
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       layoutHandle,
                                       spec.descriptorSetFirst,
                                       spec.descriptorSets,
                                       spec.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(spec.descriptorSetFirst, static_cast<uint32_t>(spec.descriptorSets.size()));
#endif
  }
  for (const auto& bind : spec.extraDescriptorBinds) {
    if (bind.sets.empty()) {
      continue;
    }
    m_commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                       layoutHandle,
                                       bind.firstSet,
                                       bind.sets,
                                       bind.dynamicOffsets);
#ifndef NDEBUG
    m_debug.markDescriptorSets(bind.firstSet, static_cast<uint32_t>(bind.sets.size()));
#endif
  }

  if (spec.pushConstantsSize > 0) {
    CHECK(spec.pushConstantsData != nullptr) << "Graphics draw push constants require data";
    const auto* bytes = static_cast<const std::uint8_t*>(spec.pushConstantsData);
    vk::ArrayProxy<const std::uint8_t> payload(spec.pushConstantsSize, bytes);
    m_commandBuffer.pushConstants(layoutHandle, spec.pushConstantsStages, 0, payload);
#ifndef NDEBUG
    m_debug.markPushConstants(spec.pushConstantsSize);
#endif
  }

  if (!spec.vertexBuffers.empty()) {
    CHECK(spec.vertexBuffers.size() == spec.vertexOffsets.size())
      << "Vertex buffers and offsets must have matching counts";
    m_commandBuffer.bindVertexBuffers(0, spec.vertexBuffers, spec.vertexOffsets);
  }
  if (spec.indexCount > 0) {
    CHECK(static_cast<VkBuffer>(spec.indexBuffer) != VK_NULL_HANDLE) << "Indexed draw missing index buffer";
    m_commandBuffer.bindIndexBuffer(spec.indexBuffer, spec.indexOffset, spec.indexType);
  }

  if (!spec.viewports.empty()) {
    m_commandBuffer.setViewport(0, spec.viewports);
#ifndef NDEBUG
    m_debug.markViewport();
#endif
  }
  if (!spec.scissors.empty()) {
    m_commandBuffer.setScissor(0, spec.scissors);
#ifndef NDEBUG
    m_debug.markScissor();
#endif
  }
  if (spec.lineWidth) {
    m_commandBuffer.setLineWidth(*spec.lineWidth);
#ifndef NDEBUG
    m_debug.markLineWidth();
#endif
  }
  if (spec.depthBiasEnable) {
    m_commandBuffer.setDepthBiasEnableEXT(*spec.depthBiasEnable);
#ifndef NDEBUG
    m_debug.markDepthBias();
#endif
  }
  if (spec.depthBiasConstant || spec.depthBiasClamp || spec.depthBiasSlope) {
    m_commandBuffer.setDepthBias(spec.depthBiasConstant.value_or(0.0f),
                                 spec.depthBiasClamp.value_or(0.0f),
                                 spec.depthBiasSlope.value_or(0.0f));
#ifndef NDEBUG
    m_debug.markDepthBias();
#endif
  }
  if (spec.blendConstants) {
    m_commandBuffer.setBlendConstants(spec.blendConstants->data());
#ifndef NDEBUG
    m_debug.markBlendConstants();
#endif
  }
  if (spec.stencilCompareMask) {
    m_commandBuffer.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilCompareMask);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.stencilWriteMask) {
    m_commandBuffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilWriteMask);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.stencilRef) {
    m_commandBuffer.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, *spec.stencilRef);
#ifndef NDEBUG
    m_debug.markStencilState();
#endif
  }
  if (spec.cullMode) {
    m_commandBuffer.setCullMode(*spec.cullMode);
#ifndef NDEBUG
    m_debug.markCullMode();
#endif
  }
  if (spec.frontFace) {
    m_commandBuffer.setFrontFace(*spec.frontFace);
#ifndef NDEBUG
    m_debug.markFrontFace();
#endif
  }
  if (spec.primitiveRestartEnable) {
    m_commandBuffer.setPrimitiveRestartEnable(*spec.primitiveRestartEnable);
#ifndef NDEBUG
    m_debug.markPrimitiveRestart();
#endif
  }
  if (spec.depthTestEnable) {
    m_commandBuffer.setDepthTestEnable(*spec.depthTestEnable);
#ifndef NDEBUG
    m_debug.markDepthTest();
#endif
  }
  if (spec.depthWriteEnable) {
    m_commandBuffer.setDepthWriteEnable(*spec.depthWriteEnable);
#ifndef NDEBUG
    m_debug.markDepthWrite();
#endif
  }
  if (spec.depthCompareOp) {
    m_commandBuffer.setDepthCompareOp(*spec.depthCompareOp);
#ifndef NDEBUG
    m_debug.markDepthCompare();
#endif
  }
  if (spec.stencilTestEnable) {
    m_commandBuffer.setStencilTestEnable(*spec.stencilTestEnable);
#ifndef NDEBUG
    m_debug.markStencilTest();
#endif
  }
  if (spec.topology) {
    m_commandBuffer.setPrimitiveTopology(*spec.topology);
#ifndef NDEBUG
    m_debug.markTopology();
#endif
  }
  if (spec.rasterizerDiscardEnable) {
    m_commandBuffer.setRasterizerDiscardEnable(*spec.rasterizerDiscardEnable);
#ifndef NDEBUG
    m_debug.markRasterizerDiscard();
#endif
  }

#ifndef NDEBUG
  m_debug.assertGraphicsPreDraw(spec);
#endif

  if (drawFn) {
    drawFn(m_commandBuffer);
  } else {
    if (spec.indexCount > 0) {
      m_commandBuffer.drawIndexed(spec.indexCount,
                                  spec.instanceCount,
                                  spec.firstIndex,
                                  spec.vertexOffset,
                                  spec.firstInstance);
    } else {
      CHECK(spec.vertexCount > 0) << "Graphics draw requires vertexCount when not indexed";
      m_commandBuffer.draw(spec.vertexCount, spec.instanceCount, spec.firstVertex, spec.firstInstance);
    }
  }
  if (auto* be = Z3DRendererVulkanBackend::current()) {
    be->notifyDrawSubmitted();
  }
}

vk::raii::CommandBuffer buildStaticSecondary(const ZVulkanSecondaryBuildInfo& info,
                                             const std::function<void(vk::raii::CommandBuffer&)>& recorder)
{
  CHECK(info.device != nullptr) << "Secondary build requires a device";
  CHECK(info.commandPool != nullptr) << "Secondary build requires a command pool";
  CHECK(static_cast<bool>(recorder)) << "Secondary build requires a recording callback";

  vk::CommandBufferAllocateInfo allocInfo{.commandPool = **info.commandPool,
                                          .level = vk::CommandBufferLevel::eSecondary,
                                          .commandBufferCount = 1};
  vk::raii::CommandBuffers buffers{*info.device, allocInfo};
  vk::raii::CommandBuffer secondary = std::move(buffers[0]);

  vk::CommandBufferBeginInfo beginInfo{.flags = info.usage, .pInheritanceInfo = &info.inheritance};
  secondary.begin(beginInfo);
  recorder(secondary);
  secondary.end();
  return secondary;
}

folly::coro::Task<vk::raii::CommandBuffer>
buildStaticSecondaryAsync(const ZVulkanSecondaryBuildInfo& info, std::function<void(vk::raii::CommandBuffer&)> recorder)
{
  co_return buildStaticSecondary(info, recorder);
}

} // namespace nim
