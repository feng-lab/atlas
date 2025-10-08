#include "zvulkantexturecopypipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "z3dtexturecopyrenderer.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbuffer.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <vector>

namespace nim {

ZVulkanTextureCopyPipelineContext::ZVulkanTextureCopyPipelineContext(Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureCopyPipelineContext::~ZVulkanTextureCopyPipelineContext() = default;

void ZVulkanTextureCopyPipelineContext::resetFrame()
{
  m_vertexCount = 0;
  resetDescriptors();
}

void ZVulkanTextureCopyPipelineContext::resetDescriptors()
{
  m_descriptorSet.reset();
}

void ZVulkanTextureCopyPipelineContext::record(Z3DRendererBase&,
                                               const RenderBatch& batch,
                                               const TextureCopyPayload& payload,
                                               const vk::Viewport& viewport,
                                               const vk::Rect2D& scissor,
                                               vk::raii::CommandBuffer& cmd)
{
  (void)payload;

  if (!payload.colorAttachmentHandle.valid() || !payload.depthAttachmentHandle.valid()) {
    LOG_FIRST_N(WARNING, 3) << "Skipping Vulkan texture copy pass due to missing attachments";
    return;
  }

  auto& colorTexture =
    vulkan::textureFromHandle(payload.colorAttachmentHandle, m_backend.device(), "texture-copy color attachment");
  auto& depthTexture =
    vulkan::textureFromHandle(payload.depthAttachmentHandle, m_backend.device(), "texture-copy depth attachment");

  // Fullscreen quad with UVs
  m_vertexCount = 4;

  ensureDescriptorLayout();
  ensureDescriptorSet();
  // Allocate per-draw override descriptor set to avoid update-after-bind
  ZVulkanDescriptorSet* ds = nullptr;
  if (m_setTextures) {
    ds = m_backend.allocateOverrideDescriptorSet(**m_setTextures);
  }
  CHECK(ds != nullptr) << "Texture copy: override descriptor allocation failed (fatal)";
  // Use nearest clamp sampler to ensure a texel-accurate copy with no filtering
  auto sampler = m_backend.nearestClampSampler();
  ds->updateTexture(0, colorTexture, sampler);
  ds->updateTexture(1, depthTexture, sampler);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.discardTransparent = payload.discardTransparent;
  key.mode = payload.mode;
  key.flipY = payload.flipY;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& pipeline = ensurePipeline(key, formats);

  // Ensure and bind local quad VBO with positions + UVs
  ensureVertexCapacity(m_vertexCount);
  uploadGeometry();

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {vk::DeviceSize(0)});

  {
    std::array<vk::DescriptorSet, 1> sets{ds->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline->pipelineLayout(), 0, sets, {});
  }

  // Use the provided viewport directly (no viewport-based Y flip)
  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  // No push constants needed; shader samples via UVs

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorLayout()
{
  if (m_setTextures) {
    return;
  }

  auto& device = m_backend.device();
  auto& vkDevice = device.context().device();

  std::array<vk::DescriptorSetLayoutBinding, 2> bindings{
    vk::DescriptorSetLayoutBinding{.binding = 0,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment},
    vk::DescriptorSetLayoutBinding{.binding = 1,
                                   .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                                   .descriptorCount = 1,
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}
  };

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setTextures.emplace(vkDevice, createInfo);
}

void ZVulkanTextureCopyPipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();

  if (!m_descriptorSet) {
    m_descriptorSet = m_backend.allocateFrameDescriptorSet(**m_setTextures);
  }
}

vk::PipelineVertexInputStateCreateInfo ZVulkanTextureCopyPipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(QuadVertex)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 2> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(QuadVertex, position))},
    vk::VertexInputAttributeDescription{.location = 1,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32Sfloat,
                                        .offset = static_cast<uint32_t>(offsetof(QuadVertex, uv))      }
  };

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanTextureCopyPipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t required = vertexCount * sizeof(QuadVertex);
  if (!m_vertexBuffer || m_vertexCapacity < required) {
    m_vertexBuffer = m_backend.device().createBuffer(required,
                                                     vk::BufferUsageFlagBits::eVertexBuffer,
                                                     vk::MemoryPropertyFlagBits::eHostVisible |
                                                       vk::MemoryPropertyFlagBits::eHostCoherent);
    m_vertexCapacity = required;
  }
}

void ZVulkanTextureCopyPipelineContext::uploadGeometry()
{
  if (!m_vertexBuffer) {
    return;
  }
  QuadVertex verts[4] = {
    {glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec2(0.0f, 0.0f)},
    {glm::vec3(1.0f,  -1.0f, 0.0f), glm::vec2(1.0f, 0.0f)},
    {glm::vec3(-1.0f, 1.0f,  0.0f), glm::vec2(0.0f, 1.0f)},
    {glm::vec3(1.0f,  1.0f,  0.0f), glm::vec2(1.0f, 1.0f)},
  };
  m_vertexBuffer->copyData(verts, sizeof(verts));
}

ZVulkanTextureCopyPipelineContext::PipelineInstance&
ZVulkanTextureCopyPipelineContext::ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase = ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  // Use single fragment with specialization constant to control Y-flip.
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass_with_2dtexture.vert.spv",
                                                    shaderBase + "copyimage.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_setTextures});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  // Depth not needed for full-screen copy
  instance.pipeline->setDepthTestEnable(false);
  instance.pipeline->setDepthWriteEnable(false);

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  std::array<vk::SpecializationMapEntry, 4> entries{
    vk::SpecializationMapEntry{.constantID = 60, .offset = 0 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 61, .offset = 1 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 62, .offset = 2 * sizeof(uint32_t), .size = sizeof(uint32_t)},
    vk::SpecializationMapEntry{.constantID = 63, .offset = 3 * sizeof(uint32_t), .size = sizeof(uint32_t)}
  };

  const bool discard = key.discardTransparent;
  const bool multiply = key.mode == TextureCopyPayload::OutputMode::MultiplyAlpha;
  const bool divide = key.mode == TextureCopyPayload::OutputMode::DivideByAlpha;

  std::array<uint32_t, 4> specData{static_cast<uint32_t>(discard),
                                   static_cast<uint32_t>(multiply),
                                   static_cast<uint32_t>(divide),
                                   static_cast<uint32_t>(key.flipY)};

  instance.shader->setSpecializationConstants(
    vk::ShaderStageFlagBits::eFragment,
    std::vector<vk::SpecializationMapEntry>(entries.begin(), entries.end()),
    std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(specData.data()),
                         reinterpret_cast<const uint8_t*>(specData.data()) + sizeof(specData)));

  // No push constants used in copy pipeline; UVs drive sampling
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim
