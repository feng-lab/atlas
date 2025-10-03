#include "zvulkantextureweightedaveragepipelinecontext.h"

#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zsysteminfo.h"
#include "zvulkandevice.h"
#include "zvulkanpipeline.h"
#include "zvulkanshader.h"
#include "zvulkantexture.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanrenderconversions.h"
#include "zvulkanbuffer.h"
#include "zlog.h"

#include <array>
#include <vector>

namespace nim {

namespace {

constexpr float kQuadDepth = 0.0f;

} // namespace

ZVulkanTextureWeightedAveragePipelineContext::ZVulkanTextureWeightedAveragePipelineContext(
  Z3DRendererVulkanBackend& backend)
  : m_backend(backend)
{}

ZVulkanTextureWeightedAveragePipelineContext::~ZVulkanTextureWeightedAveragePipelineContext() = default;

void ZVulkanTextureWeightedAveragePipelineContext::resetFrame()
{
  m_vertexCount = 0;
}

void ZVulkanTextureWeightedAveragePipelineContext::record(Z3DRendererBase& renderer,
                                                          const RenderBatch& batch,
                                                          const TextureWeightedAveragePayload& payload,
                                                          const vk::Viewport& viewport,
                                                          const vk::Rect2D& scissor,
                                                          vk::raii::CommandBuffer& cmd)
{
  if (payload.accumulationAttachment.backend != AttachmentBackend::Vulkan ||
      payload.momentsAttachment.backend != AttachmentBackend::Vulkan ||
      !payload.accumulationAttachment.valid() || !payload.momentsAttachment.valid()) {
    LOG_FIRST_N(WARNING, 5) << "Weighted average payload missing Vulkan attachment handles.";
    return;
  }

  uploadGeometry();
  if (!m_vertexBuffer || m_vertexCount == 0) {
    return;
  }

  ensureDescriptorLayout();
  ensureDescriptorPool();
  ensureDescriptorSet();
  if (!m_descriptorSet) {
    return;
  }

  auto* accumulationTexture = reinterpret_cast<ZVulkanTexture*>(payload.accumulationAttachment.id);
  auto* momentsTexture = reinterpret_cast<ZVulkanTexture*>(payload.momentsAttachment.id);
  if (!accumulationTexture || !momentsTexture) {
    return;
  }

  m_descriptorSet->updateTexture(0, *accumulationTexture);
  m_descriptorSet->updateTexture(1, *momentsTexture);

  const auto formats = vulkan::extractAttachmentFormats(batch);

  PipelineKey key;
  key.colorFormats = formats.colorFormats;
  key.depthFormat = formats.depthFormat;

  PipelineInstance& instance = ensurePipeline(key, formats);

  vk::DeviceSize offsets = 0;
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipeline());
  cmd.bindVertexBuffers(0, {m_vertexBuffer->buffer()}, {offsets});

  if (m_descriptorSet) {
    std::array<vk::DescriptorSet, 1> sets{m_descriptorSet->descriptorSet()};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, instance.pipeline->pipelineLayout(), 0, sets, {});
  }

  cmd.setViewport(0, viewport);
  cmd.setScissor(0, scissor);

  glm::vec2 extent = batch.pass.viewport.extent;
  if (extent.x <= 0.0f || extent.y <= 0.0f) {
    const auto& viewportState = renderer.frameState().viewport;
    extent = glm::vec2(static_cast<float>(viewportState.z), static_cast<float>(viewportState.w));
  }
  if (extent.x <= 0.0f) {
    extent.x = 1.0f;
  }
  if (extent.y <= 0.0f) {
    extent.y = 1.0f;
  }

  WeightedAveragePushConstants constants;
  constants.screenDimRcp = payload.screenDimRcp;
  if (constants.screenDimRcp.x <= 0.0f || constants.screenDimRcp.y <= 0.0f) {
    constants.screenDimRcp = glm::vec2(1.0f / extent.x, 1.0f / extent.y);
  }

  cmd.pushConstants<WeightedAveragePushConstants>(instance.pipeline->pipelineLayout(),
                                                  vk::ShaderStageFlagBits::eFragment,
                                                  0,
                                                  constants);

  cmd.draw(static_cast<uint32_t>(m_vertexCount), 1, 0, 0);
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorLayout()
{
  if (m_setLayout) {
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
                                   .stageFlags = vk::ShaderStageFlagBits::eFragment}};

  vk::DescriptorSetLayoutCreateInfo createInfo{.bindingCount = static_cast<uint32_t>(bindings.size()),
                                               .pBindings = bindings.data()};
  m_setLayout.emplace(vkDevice, createInfo);
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorPool()
{
  if (!m_descriptorPool) {
    m_descriptorPool = m_backend.device().createDescriptorPool();
  }
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureDescriptorSet()
{
  ensureDescriptorLayout();
  ensureDescriptorPool();

  if (!m_descriptorSet) {
    auto descriptorSet = m_descriptorPool->allocateDescriptorSet(**m_setLayout);
    m_descriptorSet = std::make_unique<ZVulkanDescriptorSet>(m_backend.device(), std::move(descriptorSet));
  }
}

vk::PipelineVertexInputStateCreateInfo
ZVulkanTextureWeightedAveragePipelineContext::makeVertexInputState() const
{
  static vk::VertexInputBindingDescription binding{.binding = 0,
                                                   .stride = static_cast<uint32_t>(sizeof(glm::vec3)),
                                                   .inputRate = vk::VertexInputRate::eVertex};
  static std::array<vk::VertexInputAttributeDescription, 1> attrs{
    vk::VertexInputAttributeDescription{.location = 0,
                                        .binding = 0,
                                        .format = vk::Format::eR32G32B32Sfloat,
                                        .offset = 0}};

  static vk::PipelineVertexInputStateCreateInfo info{};
  info.vertexBindingDescriptionCount = 1;
  info.pVertexBindingDescriptions = &binding;
  info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
  info.pVertexAttributeDescriptions = attrs.data();
  return info;
}

void ZVulkanTextureWeightedAveragePipelineContext::ensureVertexCapacity(size_t vertexCount)
{
  const size_t requiredBytes = vertexCount * sizeof(glm::vec3);
  if (requiredBytes <= m_vertexCapacity) {
    return;
  }

  size_t newCapacity = std::max(requiredBytes, m_vertexCapacity == 0 ? requiredBytes : m_vertexCapacity * 2);
  auto& device = m_backend.device();
  m_vertexBuffer = device.createBuffer(newCapacity,
                                       vk::BufferUsageFlagBits::eVertexBuffer,
                                       vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  m_vertexCapacity = newCapacity;
}

void ZVulkanTextureWeightedAveragePipelineContext::uploadGeometry()
{
  const std::array<glm::vec3, 4> vertices{
    glm::vec3(-1.f, 1.f, kQuadDepth),
    glm::vec3(-1.f, -1.f, kQuadDepth),
    glm::vec3(1.f, 1.f, kQuadDepth),
    glm::vec3(1.f, -1.f, kQuadDepth)};

  ensureVertexCapacity(vertices.size());
  if (!m_vertexBuffer) {
    return;
  }

  m_vertexBuffer->copyData(vertices.data(), vertices.size() * sizeof(glm::vec3));
  m_vertexCount = vertices.size();
}

ZVulkanTextureWeightedAveragePipelineContext::PipelineInstance&
ZVulkanTextureWeightedAveragePipelineContext::ensurePipeline(const PipelineKey& key,
                                                             const vulkan::AttachmentFormats& formats)
{
  auto it = m_pipelineCache.find(key);
  if (it != m_pipelineCache.end()) {
    return it->second;
  }

  ensureDescriptorLayout();

  auto& device = m_backend.device();
  static const std::string shaderBase =
    ZSystemInfo::resourcesDirPath().toStdString() + "/shader/vulkan/spv/";

  PipelineInstance instance;
  instance.shader = std::make_unique<ZVulkanShader>(device,
                                                    shaderBase + "pass.vert.spv",
                                                    shaderBase + "wavg_final.frag.spv",
                                                    std::nullopt);

  auto vertexInput = makeVertexInputState();
  instance.pipeline = device.createPipeline(*instance.shader, vertexInput, vk::PrimitiveTopology::eTriangleStrip);
  instance.pipeline->setDescriptorSetLayouts({**m_setLayout});
  instance.pipeline->setAttachmentFormats(formats.colorFormats, formats.depthFormat);
  instance.pipeline->setCullMode(vk::CullModeFlagBits::eNone);
  instance.pipeline->setFrontFace(vk::FrontFace::eCounterClockwise);
  instance.pipeline->setDepthTestEnable(true);
  instance.pipeline->setDepthCompareOp(vk::CompareOp::eAlways);
  instance.pipeline->setDepthWriteEnable(true);

  vk::PipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  instance.pipeline->setColorBlendAttachment(blendAttachment);

  vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                              .offset = 0,
                              .size = static_cast<uint32_t>(sizeof(WeightedAveragePushConstants))};
  instance.pipeline->setPushConstantRanges({range});
  instance.pipeline->create();

  auto [inserted, _] = m_pipelineCache.insert({key, std::move(instance)});
  return inserted->second;
}

} // namespace nim

