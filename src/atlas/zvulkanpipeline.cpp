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

  // MoltenVK/Metal portability note:
  // Metal does not support *disabling* primitive restart for strip/fan topologies, and MoltenVK emits:
  //   VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive restart.
  //
  // Align Vulkan pipeline state with the underlying platform behavior by enabling primitive restart
  // for topologies where it is relevant. This removes noisy warnings during pipeline creation on macOS
  // without changing semantics on Metal (primitive restart cannot be disabled there anyway).
  const bool primitiveRestartEnable =
#if defined(__APPLE__)
    (m_topology == vk::PrimitiveTopology::eLineStrip) || (m_topology == vk::PrimitiveTopology::eTriangleStrip) ||
    (m_topology == vk::PrimitiveTopology::eTriangleFan) ||
    (m_topology == vk::PrimitiveTopology::eLineStripWithAdjacency) ||
    (m_topology == vk::PrimitiveTopology::eTriangleStripWithAdjacency);
#else
    false;
#endif

  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = m_topology,
                                                         .primitiveRestartEnable =
                                                           static_cast<vk::Bool32>(primitiveRestartEnable)};

  vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 1,
                                                    .pViewports = nullptr,
                                                    .scissorCount = 1,
                                                    .pScissors = nullptr};

  vk::PipelineRasterizationStateCreateInfo rasterizer{.depthClampEnable = false,
                                                      .rasterizerDiscardEnable = false,
                                                      .polygonMode = m_polygonMode,
                                                      .cullMode = m_cullMode,
                                                      .frontFace = m_frontFace,
                                                      .depthBiasEnable = static_cast<vk::Bool32>(m_depthBiasEnable),
                                                      .depthBiasConstantFactor = m_depthBiasConstant,
                                                      .depthBiasClamp = 0.0f,
                                                      .depthBiasSlopeFactor = m_depthBiasSlope,
                                                      .lineWidth = m_lineWidth};

  vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1,
                                                       .sampleShadingEnable = false,
                                                       .minSampleShading = 1.0f,
                                                       .pSampleMask = nullptr,
                                                       .alphaToCoverageEnable = false,
                                                       .alphaToOneEnable = false};

  vk::PipelineDepthStencilStateCreateInfo depthStencil{.depthTestEnable = static_cast<vk::Bool32>(m_depthTestEnable),
                                                       .depthWriteEnable = static_cast<vk::Bool32>(m_depthWriteEnable),
                                                       .depthCompareOp = m_depthCompareOp,
                                                       .depthBoundsTestEnable = false,
                                                       .stencilTestEnable = false,
                                                       .front = {},
                                                       .back = {},
                                                       .minDepthBounds = 0.0f,
                                                       .maxDepthBounds = 1.0f};

  const uint32_t colorAttachmentCount = static_cast<uint32_t>(m_colorAttachmentFormats.size());
  if (colorAttachmentCount == 0u) {
    // No color attachments: Vulkan requires attachmentCount=0 and pAttachments=null.
    // Keep m_colorBlendAttachments as-is (some callers never touch it) but do not
    // feed it into pipeline creation.
  } else {
    // Ensure there is at least one blend state and replicate to cover all
    // attachments. This keeps pipeline creation robust for passes that bind
    // multiple render targets or use depth-only passes (count=0).
    if (m_colorBlendAttachments.empty()) {
      vk::PipelineColorBlendAttachmentState att{};
      att.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                           vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
      att.blendEnable = VK_FALSE;
      m_colorBlendAttachments.push_back(att);
    }
    if (m_colorBlendAttachments.size() < colorAttachmentCount) {
      m_colorBlendAttachments.resize(colorAttachmentCount, m_colorBlendAttachments.back());
    }
  }

  const uint32_t blendAttachmentCount = (colorAttachmentCount == 0u) ? 0u : colorAttachmentCount;
  const vk::PipelineColorBlendAttachmentState* blendAttachmentPtr =
    blendAttachmentCount == 0u ? nullptr : m_colorBlendAttachments.data();
  vk::PipelineColorBlendStateCreateInfo colorBlending{
    .logicOpEnable = false,
    .logicOp = vk::LogicOp::eCopy,
    .attachmentCount = blendAttachmentCount,
    .pAttachments = blendAttachmentPtr,
    .blendConstants = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
  };

  std::array<vk::DynamicState, 2> dynFixed = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
  // Do not use VK_EXT_vertex_input_dynamic_state; rely on static vertex input descriptions.
  const vk::DynamicState* dynPtr = dynFixed.data();
  const uint32_t dynCount = static_cast<uint32_t>(dynFixed.size());
  vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = dynCount, .pDynamicStates = dynPtr};

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
