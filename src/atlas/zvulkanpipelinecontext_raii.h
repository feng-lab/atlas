#pragma once

#include "zvulkan.h"
#include "zlog.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <folly/experimental/coro/Task.h>

namespace nim {

class ZVulkanTexture;

struct ZVulkanAttachmentInfo
{
  vk::Image image{};
  vk::ImageView view{};
  vk::Format format{vk::Format::eUndefined};
  vk::ImageLayout initialLayout{vk::ImageLayout::eUndefined};
  vk::ImageLayout finalLayout{vk::ImageLayout::eUndefined};
  vk::ClearValue clearValue{};
  vk::AttachmentLoadOp loadOp{vk::AttachmentLoadOp::eClear};
  vk::AttachmentStoreOp storeOp{vk::AttachmentStoreOp::eStore};
  vk::AccessFlags2 srcAccess{};
  vk::AccessFlags2 dstAccess{};
  vk::PipelineStageFlags2 srcStage{};
  vk::PipelineStageFlags2 dstStage{};
  vk::ImageAspectFlags aspect{vk::ImageAspectFlagBits::eColor};
  ZVulkanTexture* trackingTexture{nullptr};
};

struct ZVulkanDescriptorBindInfo
{
  uint32_t firstSet{0};
  std::vector<vk::DescriptorSet> sets{};
  std::vector<uint32_t> dynamicOffsets{};
};

struct ZVulkanGraphicsPassSpec
{
  vk::Rect2D renderArea{};
  std::vector<vk::Viewport> viewports{};
  std::vector<vk::Rect2D> scissors{};

  std::vector<ZVulkanAttachmentInfo> colorAttachments{};
  std::optional<ZVulkanAttachmentInfo> depthStencilAttachment{};

  const vk::raii::Pipeline* pipeline{nullptr};
  const vk::raii::PipelineLayout* pipelineLayout{nullptr};
  vk::Pipeline pipelineHandle{VK_NULL_HANDLE};
  vk::PipelineLayout pipelineLayoutHandle{VK_NULL_HANDLE};
  std::vector<vk::DescriptorSet> descriptorSets{};
  std::vector<uint32_t> dynamicOffsets{};
  std::optional<uint32_t> expectedDescriptorSetCount{};
  uint32_t descriptorSetFirst{0};
  std::vector<ZVulkanDescriptorBindInfo> extraDescriptorBinds{};

  std::vector<vk::Buffer> vertexBuffers{};
  std::vector<vk::DeviceSize> vertexOffsets{};
  vk::Buffer indexBuffer{};
  vk::DeviceSize indexOffset{0};
  vk::IndexType indexType{vk::IndexType::eUint32};

  const void* pushConstantsData{nullptr};
  uint32_t pushConstantsSize{0};
  vk::ShaderStageFlags pushConstantsStages{};
  bool requirePushConstants{false};

  std::optional<float> lineWidth{};
  std::optional<vk::Bool32> depthBiasEnable{};
  std::optional<float> depthBiasConstant{};
  std::optional<float> depthBiasClamp{};
  std::optional<float> depthBiasSlope{};
  std::optional<std::array<float, 4>> blendConstants{};
  std::optional<uint32_t> stencilCompareMask{};
  std::optional<uint32_t> stencilWriteMask{};
  std::optional<uint32_t> stencilRef{};
  std::optional<vk::CullModeFlags> cullMode{};
  std::optional<vk::FrontFace> frontFace{};
  std::optional<vk::Bool32> primitiveRestartEnable{};
  std::optional<vk::Bool32> depthTestEnable{};
  std::optional<vk::Bool32> depthWriteEnable{};
  std::optional<vk::CompareOp> depthCompareOp{};
  std::optional<vk::Bool32> stencilTestEnable{};
  std::optional<vk::PrimitiveTopology> topology{};
  std::optional<vk::Bool32> rasterizerDiscardEnable{};

  uint32_t indexCount{0};
  uint32_t instanceCount{1};
  uint32_t firstIndex{0};
  uint32_t firstInstance{0};
  int32_t vertexOffset{0};
  uint32_t vertexCount{0};
  uint32_t firstVertex{0};

  bool allowActiveQueries{false};
  bool allowConditionalRendering{false};
  bool activeQueries{false};
  bool conditionalRendering{false};
};

struct ZVulkanComputePassSpec
{
  const vk::raii::Pipeline* pipeline{nullptr};
  const vk::raii::PipelineLayout* pipelineLayout{nullptr};
  std::vector<vk::DescriptorSet> descriptorSets{};
  std::vector<uint32_t> dynamicOffsets{};
  std::optional<uint32_t> expectedDescriptorSetCount{};
  uint32_t descriptorSetFirst{0};
  std::vector<ZVulkanDescriptorBindInfo> extraDescriptorBinds{};
  const void* pushConstantsData{nullptr};
  uint32_t pushConstantsSize{0};
  vk::ShaderStageFlags pushConstantsStages{};
  bool requirePushConstants{false};
  uint32_t groupX{1};
  uint32_t groupY{1};
  uint32_t groupZ{1};
  bool allowActiveQueries{false};
  bool allowConditionalRendering{false};
  bool activeQueries{false};
  bool conditionalRendering{false};
};

#ifndef NDEBUG
bool atlasVulkanPipelineContextEnforcementEnabled();
#endif

struct ZVulkanRenderingSegmentSpec
{
  vk::Rect2D renderArea{};
  std::vector<ZVulkanAttachmentInfo> colorAttachments{};
  std::optional<ZVulkanAttachmentInfo> depthStencilAttachment{};
};

struct ZVulkanGraphicsDrawSpec
{
  const vk::raii::Pipeline* pipeline{nullptr};
  const vk::raii::PipelineLayout* pipelineLayout{nullptr};
  vk::Pipeline pipelineHandle{VK_NULL_HANDLE};
  vk::PipelineLayout pipelineLayoutHandle{VK_NULL_HANDLE};

  std::vector<vk::DescriptorSet> descriptorSets{};
  std::vector<uint32_t> dynamicOffsets{};
  std::optional<uint32_t> expectedDescriptorSetCount{};
  uint32_t descriptorSetFirst{0};
  std::vector<ZVulkanDescriptorBindInfo> extraDescriptorBinds{};

  std::vector<vk::Buffer> vertexBuffers{};
  std::vector<vk::DeviceSize> vertexOffsets{};
  vk::Buffer indexBuffer{};
  vk::DeviceSize indexOffset{0};
  vk::IndexType indexType{vk::IndexType::eUint32};

  const void* pushConstantsData{nullptr};
  uint32_t pushConstantsSize{0};
  vk::ShaderStageFlags pushConstantsStages{};
  bool requirePushConstants{false};

  std::vector<vk::Viewport> viewports{};
  std::vector<vk::Rect2D> scissors{};

  std::optional<float> lineWidth{};
  std::optional<vk::Bool32> depthBiasEnable{};
  std::optional<float> depthBiasConstant{};
  std::optional<float> depthBiasClamp{};
  std::optional<float> depthBiasSlope{};
  std::optional<std::array<float, 4>> blendConstants{};
  std::optional<uint32_t> stencilCompareMask{};
  std::optional<uint32_t> stencilWriteMask{};
  std::optional<uint32_t> stencilRef{};
  std::optional<vk::CullModeFlags> cullMode{};
  std::optional<vk::FrontFace> frontFace{};
  std::optional<vk::Bool32> primitiveRestartEnable{};
  std::optional<vk::Bool32> depthTestEnable{};
  std::optional<vk::Bool32> depthWriteEnable{};
  std::optional<vk::CompareOp> depthCompareOp{};
  std::optional<vk::Bool32> stencilTestEnable{};
  std::optional<vk::PrimitiveTopology> topology{};
  std::optional<vk::Bool32> rasterizerDiscardEnable{};

  // Draw counts
  uint32_t indexCount{0};
  uint32_t instanceCount{1};
  uint32_t firstIndex{0};
  uint32_t firstInstance{0};
  int32_t vertexOffset{0};
  uint32_t vertexCount{0};
  uint32_t firstVertex{0};

  bool allowActiveQueries{false};
  bool allowConditionalRendering{false};
  bool activeQueries{false};
  bool conditionalRendering{false};
};

#ifndef NDEBUG
class ZVulkanDebugStateTracker
{
public:
  void reset(const ZVulkanGraphicsPassSpec& spec);
  void reset(const ZVulkanComputePassSpec& spec);
  // Draw-only reset (no attachments) for segment-managed flows
  void reset(const ZVulkanGraphicsDrawSpec& spec);

  void markViewport();
  void markScissor();
  void markLineWidth();
  void markDepthBias();
  void markBlendConstants();
  void markStencilState();
  void markCullMode();
  void markFrontFace();
  void markPrimitiveRestart();
  void markDepthTest();
  void markDepthWrite();
  void markDepthCompare();
  void markStencilTest();
  void markTopology();
  void markRasterizerDiscard();
  void markDescriptorSets(uint32_t firstSet, uint32_t boundCount);
  void markPushConstants(uint32_t size);

  void assertGraphicsPreDraw(const ZVulkanGraphicsPassSpec& spec) const;
  void assertComputePreDispatch(const ZVulkanComputePassSpec& spec) const;
  // Draw-only assertion counterpart
  void assertGraphicsPreDraw(const ZVulkanGraphicsDrawSpec& spec) const;

private:
  void require(bool condition, const char* message) const;

  bool m_expectedViewport = false;
  bool m_expectedScissor = false;
  bool m_expectedLineWidth = false;
  bool m_expectedDepthBias = false;
  bool m_expectedBlendConstants = false;
  bool m_expectedStencilState = false;
  bool m_expectedCullMode = false;
  bool m_expectedFrontFace = false;
  bool m_expectedPrimitiveRestart = false;
  bool m_expectedDepthTest = false;
  bool m_expectedDepthWrite = false;
  bool m_expectedDepthCompare = false;
  bool m_expectedStencilTest = false;
  bool m_expectedTopology = false;
  bool m_expectedRasterizerDiscard = false;
  std::optional<uint32_t> m_expectedDescriptorSets{};
  bool m_expectPushConstants = false;

  bool m_viewportKnown = false;
  bool m_scissorKnown = false;
  bool m_lineWidthKnown = false;
  bool m_depthBiasKnown = false;
  bool m_blendConstantsKnown = false;
  bool m_stencilStateKnown = false;
  bool m_cullModeKnown = false;
  bool m_frontFaceKnown = false;
  bool m_primitiveRestartKnown = false;
  bool m_depthTestKnown = false;
  bool m_depthWriteKnown = false;
  bool m_depthCompareKnown = false;
  bool m_stencilTestKnown = false;
  bool m_topologyKnown = false;
  bool m_rasterizerDiscardKnown = false;
  bool m_descriptorSetsBoundAny = false;
  uint32_t m_maxDescriptorSetIndexBound = 0;
  bool m_pushConstantsKnown = false;
  uint32_t m_pushConstantsSize = 0;
  bool m_allowQueries = false;
  bool m_allowConditional = false;
};
#endif // NDEBUG

class ZVulkanPipelineCommandRecorder
{
public:
  explicit ZVulkanPipelineCommandRecorder(vk::raii::CommandBuffer& commandBuffer);

  void recordGraphicsPass(const ZVulkanGraphicsPassSpec& spec,
                          const std::function<void(vk::raii::CommandBuffer&)>& drawFn = {});
  void recordComputePass(const ZVulkanComputePassSpec& spec);

  // Segment-managed API (backend/compositor): begin/end segment once, then issue draw-only commands.
  using RenderingSegmentSpec = ZVulkanRenderingSegmentSpec;
  void beginRenderingSegment(const ZVulkanRenderingSegmentSpec& spec);
  void endRenderingSegment(const ZVulkanRenderingSegmentSpec& spec);

  // Draw-only API for pipeline contexts under an active segment
  using GraphicsDrawSpec = ZVulkanGraphicsDrawSpec;
  void recordGraphicsDraw(const ZVulkanGraphicsDrawSpec& spec,
                          const std::function<void(vk::raii::CommandBuffer&)>& drawFn = {});

private:
  static vk::ImageLayout depthAttachmentLayoutForAspect(vk::ImageAspectFlags aspect);
  static void validateAttachmentInfo(const ZVulkanAttachmentInfo& info, const char* role);
  static void validateDescriptorSets(const ZVulkanGraphicsPassSpec& spec);
  static void validateDescriptorSets(const ZVulkanComputePassSpec& spec);
  static void validateDescriptorSets(const ZVulkanGraphicsDrawSpec& spec);

  vk::raii::CommandBuffer& m_commandBuffer;
#ifndef NDEBUG
  ZVulkanDebugStateTracker m_debug{};
#endif
  bool m_segmentActive = false;
};

struct ZVulkanSecondaryBuildInfo
{
  vk::raii::Device* device{};
  vk::raii::CommandPool* commandPool{};
  vk::CommandBufferInheritanceInfo inheritance{};
  vk::CommandBufferUsageFlags usage{vk::CommandBufferUsageFlagBits::eRenderPassContinue |
                                    vk::CommandBufferUsageFlagBits::eSimultaneousUse};
};

vk::raii::CommandBuffer buildStaticSecondary(const ZVulkanSecondaryBuildInfo& info,
                                             const std::function<void(vk::raii::CommandBuffer&)>& recorder);

folly::coro::Task<vk::raii::CommandBuffer>
buildStaticSecondaryAsync(const ZVulkanSecondaryBuildInfo& info,
                          std::function<void(vk::raii::CommandBuffer&)> recorder);

} // namespace nim
