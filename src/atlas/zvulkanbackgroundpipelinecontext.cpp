#include "zvulkanbackgroundpipelinecontext.h"

#include "z3dbackgroundrenderer.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkanbuffer.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zexception.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace nim {

namespace {

struct BackgroundPushConstants
{
  glm::vec2 screen_dim_RCP{0.0f};
  glm::vec2 pad0{0.0f};
  glm::vec4 color1{1.0f};
  glm::vec4 color2{0.0f};
  glm::vec4 region{0.0f, 1.0f, 0.0f, 1.0f};
};

} // namespace

ZVulkanBackgroundPipelineContext::ZVulkanBackgroundPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanBackgroundPipelineContext::~ZVulkanBackgroundPipelineContext() = default;

void ZVulkanBackgroundPipelineContext::record(Z3DRendererBase& renderer,
                                              const RenderBatch& batch,
                                              const BackgroundPayload& payload,
                                              const vk::Viewport& viewport,
                                              const vk::Rect2D& scissor,
                                              vk::raii::CommandBuffer& cmd)
{
  (void)renderer;
  (void)payload;

  // Shared fullscreen quad geometry
  const uint32_t vertexCount = 4;

  // No descriptor sets/UBOs needed for background; only push constants/spec constants are used.

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.mode = payload.mode;
  key.orientation = payload.orientation;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  auto& quad = m_backend.fullscreenQuadVertexBuffer();

  const glm::vec2 extent(viewport.width, viewport.height);
  CHECK(extent.x > 0.0f && extent.y > 0.0f) << "Vulkan background pass requires a valid viewport extent";

  BackgroundPushConstants constants;
  constants.screen_dim_RCP = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  constants.color1 = payload.color1;
  constants.color2 = payload.color2;
  constants.region = payload.region;



  ZVulkanPipelineCommandRecorder::GraphicsDrawSpec drawSpec{};
  drawSpec.viewports = std::span<const vk::Viewport>(&viewport, 1);
  drawSpec.scissors = std::span<const vk::Rect2D>(&scissor, 1);
  drawSpec.pipelineHandle = pipeline.pipeline->pipelineHandle();
  drawSpec.pipelineLayoutHandle = pipeline.pipeline->pipelineLayoutHandle();

  const std::array<vk::Buffer, 1> vertexBuffers{quad.buffer()};
  const std::array<vk::DeviceSize, 1> vertexOffsets{vk::DeviceSize(0)};
  drawSpec.vertexBuffers = vertexBuffers;
  drawSpec.vertexOffsets = vertexOffsets;
  drawSpec.vertexCount = vertexCount;
  drawSpec.instanceCount = 1;
  drawSpec.pushConstantsData = &constants;
  drawSpec.pushConstantsSize = static_cast<uint32_t>(sizeof(BackgroundPushConstants));
  drawSpec.pushConstantsStages = vk::ShaderStageFlagBits::eFragment;
  drawSpec.requirePushConstants = true;

  ZVulkanPipelineCommandRecorder recorder(cmd);
  recorder.recordGraphicsDraw(drawSpec);

}

ZVulkanBackgroundPipelineContext::PipelineInstance&
ZVulkanBackgroundPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  auto& device = m_backend.device();

  // No descriptor sets are used for background; pipelines bind only push constants

  PipelineInstance instance;
  instance.shader =
    std::make_unique<ZVulkanShader>(device,
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                    ZVulkanShader::spirvResourcePath(QStringLiteral("background.frag.spv")),
                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  std::vector<vk::DescriptorSetLayout> layouts{}; // no descriptor sets needed
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setDescriptorSetLayouts(layouts);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Background should not participate in depth testing/writes
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  std::array<vk::SpecializationMapEntry, 5> entries{
    vk::SpecializationMapEntry{.constantID = 30, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 31, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 32, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 33, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 34, .offset = 4 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };

  const bool isUniform = key.mode == BackgroundMode::Uniform;
  const bool gradientL2R =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::LeftToRight;
  const bool gradientR2L =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::RightToLeft;
  const bool gradientT2B =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::TopToBottom;
  const bool gradientB2T =
    key.mode == BackgroundMode::Gradient && key.orientation == BackgroundGradientOrientation::BottomToTop;

  std::array<uint32_t, 5> specData{static_cast<uint32_t>(isUniform),
                                   static_cast<uint32_t>(gradientL2R),
                                   static_cast<uint32_t>(gradientR2L),
                                   static_cast<uint32_t>(gradientT2B),
                                   static_cast<uint32_t>(gradientB2T)};

  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(BackgroundPushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

vk::PipelineVertexInputStateCreateInfo ZVulkanBackgroundPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 1> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0}
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

} // namespace nim
