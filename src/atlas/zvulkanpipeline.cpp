#include "zvulkanpipeline.h"

#include "zvulkanshader.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "z3drenderervulkanbackend.h"
#include "zexception.h"
#include "zlog.h"

#include <array>

namespace nim {

ZVulkanPipeline::ZVulkanPipeline(ZVulkanDevice& device,
                                 ZVulkanShader& shader,
                                 const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                                 const vk::PrimitiveTopology& topology)
  : m_device(device)
  , m_shader(shader)
  , m_vertexInputInfo(vertexInputInfo)
  , m_topology(topology)
{}

void ZVulkanPipeline::setDescriptorSetLayouts(const std::vector<vk::DescriptorSetLayout>& layouts)
{
  m_descriptorSetLayouts = layouts;
}

void ZVulkanPipeline::setPushConstantRanges(const std::vector<vk::PushConstantRange>& pushConstantRanges)
{
  m_pushConstantRanges = pushConstantRanges;
}

void ZVulkanPipeline::setPolygonMode(vk::PolygonMode mode)
{
  m_polygonMode = mode;
}

void ZVulkanPipeline::setCullMode(vk::CullModeFlags mode)
{
  m_cullMode = mode;
}

void ZVulkanPipeline::setFrontFace(vk::FrontFace frontFace)
{
  m_frontFace = frontFace;
}

void ZVulkanPipeline::setDepthBias(bool enable, float constantFactor, float slopeFactor)
{
  m_depthBiasEnable = enable;
  m_depthBiasConstant = constantFactor;
  m_depthBiasSlope = slopeFactor;
}

void ZVulkanPipeline::setLineWidth(float width)
{
  m_lineWidth = width;
}

void ZVulkanPipeline::setDepthWriteEnable(bool enable)
{
  m_depthWriteEnable = enable;
}

void ZVulkanPipeline::setDepthTestEnable(bool enable)
{
  m_depthTestEnable = enable;
}

void ZVulkanPipeline::setDepthCompareOp(vk::CompareOp compareOp)
{
  m_depthCompareOp = compareOp;
}

void ZVulkanPipeline::setColorBlendAttachment(const vk::PipelineColorBlendAttachmentState& attachment)
{
  m_colorBlendAttachments.clear();
  m_colorBlendAttachments.push_back(attachment);
}

void ZVulkanPipeline::setColorBlendAttachments(std::vector<vk::PipelineColorBlendAttachmentState> attachments)
{
  if (attachments.empty()) {
    m_colorBlendAttachments.clear();
    return;
  }
  m_colorBlendAttachments = std::move(attachments);
}

void ZVulkanPipeline::setAttachmentFormats(std::vector<vk::Format> colorFormats, std::optional<vk::Format> depthFormat)
{
  m_colorAttachmentFormats = std::move(colorFormats);
  m_depthAttachmentFormat = std::move(depthFormat);
}

void ZVulkanPipeline::create()
{
  vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
    .setLayoutCount = static_cast<uint32_t>(m_descriptorSetLayouts.size()),
    .pSetLayouts = m_descriptorSetLayouts.empty() ? nullptr : m_descriptorSetLayouts.data(),
    .pushConstantRangeCount = static_cast<uint32_t>(m_pushConstantRanges.size()),
    .pPushConstantRanges = m_pushConstantRanges.empty() ? nullptr : m_pushConstantRanges.data()};
  m_pipelineLayout.emplace(m_device.context().device(), pipelineLayoutInfo);

  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = m_topology, .primitiveRestartEnable = VK_FALSE};

  vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 1,
                                                    .pViewports = nullptr,
                                                    .scissorCount = 1,
                                                    .pScissors = nullptr};

  vk::PipelineRasterizationStateCreateInfo rasterizer{.depthClampEnable = VK_FALSE,
                                                      .rasterizerDiscardEnable = VK_FALSE,
                                                      .polygonMode = m_polygonMode,
                                                      .cullMode = m_cullMode,
                                                      .frontFace = m_frontFace,
                                                      .depthBiasEnable = static_cast<vk::Bool32>(m_depthBiasEnable),
                                                      .depthBiasConstantFactor = m_depthBiasConstant,
                                                      .depthBiasClamp = 0.0f,
                                                      .depthBiasSlopeFactor = m_depthBiasSlope,
                                                      .lineWidth = m_lineWidth};

  vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1,
                                                       .sampleShadingEnable = VK_FALSE,
                                                       .minSampleShading = 1.0f,
                                                       .pSampleMask = nullptr,
                                                       .alphaToCoverageEnable = VK_FALSE,
                                                       .alphaToOneEnable = VK_FALSE};

  vk::PipelineDepthStencilStateCreateInfo depthStencil{.depthTestEnable = static_cast<vk::Bool32>(m_depthTestEnable),
                                                       .depthWriteEnable = static_cast<vk::Bool32>(m_depthWriteEnable),
                                                       .depthCompareOp = m_depthCompareOp,
                                                       .depthBoundsTestEnable = VK_FALSE,
                                                       .stencilTestEnable = VK_FALSE,
                                                       .front = {},
                                                       .back = {},
                                                       .minDepthBounds = 0.0f,
                                                       .maxDepthBounds = 1.0f};

  if (m_colorAttachmentFormats.size() > m_colorBlendAttachments.size() && !m_colorBlendAttachments.empty()) {
    m_colorBlendAttachments.resize(m_colorAttachmentFormats.size(), m_colorBlendAttachments.back());
  }
  const uint32_t blendAttachmentCount = static_cast<uint32_t>(m_colorBlendAttachments.size());
  const vk::PipelineColorBlendAttachmentState* blendAttachmentPtr =
    blendAttachmentCount == 0u ? nullptr : m_colorBlendAttachments.data();
  vk::PipelineColorBlendStateCreateInfo colorBlending{
    .logicOpEnable = VK_FALSE,
    .logicOp = vk::LogicOp::eCopy,
    .attachmentCount = blendAttachmentCount,
    .pAttachments = blendAttachmentPtr,
    .blendConstants = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
  };

  std::array<vk::DynamicState, 3> dynamicStates = {vk::DynamicState::eViewport,
                                                   vk::DynamicState::eScissor,
                                                   vk::DynamicState::eVertexInputEXT};
  vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
                                                  .pDynamicStates = dynamicStates.data()};

  const auto& shaderStages = m_shader.shaderStages();

  const vk::Format* colorFormatsPtr = m_colorAttachmentFormats.empty() ? nullptr : m_colorAttachmentFormats.data();
  vk::PipelineRenderingCreateInfo renderingCreateInfo{};
  renderingCreateInfo.colorAttachmentCount = static_cast<uint32_t>(m_colorAttachmentFormats.size());
  renderingCreateInfo.pColorAttachmentFormats = colorFormatsPtr;
  renderingCreateInfo.depthAttachmentFormat =
    m_depthAttachmentFormat.has_value() ? *m_depthAttachmentFormat : vk::Format::eUndefined;

  vk::GraphicsPipelineCreateInfo pipelineInfo{.pNext = &renderingCreateInfo,
                                              .stageCount = static_cast<uint32_t>(shaderStages.size()),
                                              .pStages = shaderStages.data(),
                                              .pVertexInputState = &m_vertexInputInfo,
                                              .pInputAssemblyState = &inputAssembly,
                                              .pTessellationState = nullptr,
                                              .pViewportState = &viewportState,
                                              .pRasterizationState = &rasterizer,
                                              .pMultisampleState = &multisampling,
                                              .pDepthStencilState = &depthStencil,
                                              .pColorBlendState = &colorBlending,
                                              .pDynamicState = &dynamicState,
                                              .layout = *m_pipelineLayout,
                                              .renderPass = nullptr,
                                              .subpass = 0,
                                              .basePipelineHandle = nullptr,
                                              .basePipelineIndex = -1};

  try {
    m_pipeline.emplace(m_device.context().device(), nullptr, pipelineInfo);
    LOG(INFO) << "Successfully created graphics pipeline";
    // Stage 3 instrumentation: count pipelines created in the current Vulkan backend frame
    if (auto* backend = Z3DRendererVulkanBackend::current()) {
      backend->notifyPipelineCreated();
    }
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Failed to create graphics pipeline: " << e.what();
    throw ZException(fmt::format("Failed to create graphics pipeline: {}", e.what()));
  }
}

} // namespace nim
