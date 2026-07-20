#include "zvulkanbackgroundpipelinecontext.h"

#include "z3dbackgroundrenderer.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkanpipeline.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanpipelinecontext_raii.h"
#include "zvulkanshader.h"

#include <array>
#include <cstddef>
#include <utility>

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

  // Shared fullscreen quad geometry
  const uint32_t vertexCount = 4;

  // No descriptor sets/UBOs needed for background; only push constants/spec constants are used.

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.mode = payload.mode;
  key.orientation = payload.orientation;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  auto& pipeline = ensurePipeline(key);

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
ZVulkanBackgroundPipelineContext::ensurePipeline(const PipelineKey& key)
{
  const auto found = m_pipelines.find(key);
  if (found != m_pipelines.end()) {
    return found->second;
  }

  auto& device = m_backend.device();
  auto shader = std::make_unique<ZVulkanShader>(device,
                                                ZVulkanShader::spirvResourcePath(QStringLiteral("pass.vert.spv")),
                                                ZVulkanShader::spirvResourcePath(QStringLiteral("background.frag.spv")),
                                                std::nullopt);

  const std::array vertexBindings{
    vk::VertexInputBindingDescription{.binding = 0,
                                      .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                      .inputRate = vk::VertexInputRate::eVertex}
  };
  const std::array vertexAttributes{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0}
  };
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings.size());
  vertexInput.pVertexBindingDescriptions = vertexBindings.data();
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size());
  vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

  auto pipeline = device.createPipeline(*shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  pipeline->setAttachmentFormats(key.colorFormats, key.depthFormat);
  pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  pipeline->setDepthTestEnable(false);
  pipeline->setDepthWriteEnable(false);
  pipeline->setColorBlendAttachments(
    std::vector<vk::PipelineColorBlendAttachmentState>(key.colorFormats.size(),
                                                       vulkan::toVkBlendAttachment(BlendState{})));

  const std::array<vk::SpecializationMapEntry, 5> entries{
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

  const std::array<uint32_t, 5> specData{static_cast<uint32_t>(isUniform),
                                         static_cast<uint32_t>(gradientL2R),
                                         static_cast<uint32_t>(gradientR2L),
                                         static_cast<uint32_t>(gradientT2B),
                                         static_cast<uint32_t>(gradientB2T)};

  shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  pipeline->setPushConstantRanges({
    vk::PushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                          .offset = 0,
                          .size = static_cast<uint32_t>(sizeof(BackgroundPushConstants))}
  });
  pipeline->create();

  PipelineInstance instance;
  instance.shader = std::move(shader);
  instance.pipeline = std::move(pipeline);
  auto [inserted, didInsert] = m_pipelines.emplace(key, std::move(instance));
  CHECK(didInsert) << "Vulkan background pipeline insertion failed after a cache miss";
  return inserted->second;
}

} // namespace nim
