#include "zvulkanpipeline.h"

#include "zvulkanshader.h"
#include "zvulkandevice.h"
#include "zvulkancontext.h"
#include "zexception.h"
#include "zlog.h"

#include <array>
#include <fmt/format.h>

namespace nim {

ZVulkanPipeline::ZVulkanPipeline(ZVulkanDevice& device,
                                 ZVulkanShader& shader,
                                 const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                                 const vk::PrimitiveTopology& topology)
  : m_device(device)
  , m_shader(shader)
  , m_vertexInputInfo(vertexInputInfo)
  , m_topology(topology)
{
  LOG(INFO) << "ZVulkanPipeline created";
}

ZVulkanPipeline::~ZVulkanPipeline()
{
  LOG(INFO) << "Destroying ZVulkanPipeline";
}

void ZVulkanPipeline::setDescriptorSetLayouts(const std::vector<vk::DescriptorSetLayout>& layouts)
{
  m_descriptorSetLayouts = layouts;
}

void ZVulkanPipeline::setPushConstantRanges(const std::vector<vk::PushConstantRange>& pushConstantRanges)
{
  m_pushConstantRanges = pushConstantRanges;
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
                                                      .polygonMode = vk::PolygonMode::eFill,
                                                      .cullMode = vk::CullModeFlagBits::eBack,
                                                      .frontFace = vk::FrontFace::eCounterClockwise,
                                                      .depthBiasEnable = VK_FALSE,
                                                      .depthBiasConstantFactor = 0.0f,
                                                      .depthBiasClamp = 0.0f,
                                                      .depthBiasSlopeFactor = 0.0f,
                                                      .lineWidth = 1.0f};

  vk::PipelineMultisampleStateCreateInfo multisampling{.rasterizationSamples = vk::SampleCountFlagBits::e1,
                                                       .sampleShadingEnable = VK_FALSE,
                                                       .minSampleShading = 1.0f,
                                                       .pSampleMask = nullptr,
                                                       .alphaToCoverageEnable = VK_FALSE,
                                                       .alphaToOneEnable = VK_FALSE};

  vk::PipelineDepthStencilStateCreateInfo depthStencil{.depthTestEnable = VK_TRUE,
                                                       .depthWriteEnable = VK_TRUE,
                                                       .depthCompareOp = vk::CompareOp::eLess,
                                                       .depthBoundsTestEnable = VK_FALSE,
                                                       .stencilTestEnable = VK_FALSE,
                                                       .front = {},
                                                       .back = {},
                                                       .minDepthBounds = 0.0f,
                                                       .maxDepthBounds = 1.0f};

  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
    .blendEnable = VK_TRUE,
    .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
    .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
    .colorBlendOp = vk::BlendOp::eAdd,
    .srcAlphaBlendFactor = vk::BlendFactor::eOne,
    .dstAlphaBlendFactor = vk::BlendFactor::eZero,
    .alphaBlendOp = vk::BlendOp::eAdd,
    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB |
                      vk::ColorComponentFlagBits::eA};

  vk::PipelineColorBlendStateCreateInfo colorBlending{
    .logicOpEnable = VK_FALSE,
    .logicOp = vk::LogicOp::eCopy,
    .attachmentCount = 1,
    .pAttachments = &colorBlendAttachment,
    .blendConstants = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
  };

  std::array<vk::DynamicState, 2> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
  vk::PipelineDynamicStateCreateInfo dynamicState{.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
                                                  .pDynamicStates = dynamicStates.data()};

  const auto& shaderStages = m_shader.shaderStages();

  vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm;
  vk::Format depthFormat = vk::Format::eD32Sfloat;

  vk::PipelineRenderingCreateInfo renderingCreateInfo{};
  renderingCreateInfo.colorAttachmentCount = 1;
  renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
  renderingCreateInfo.depthAttachmentFormat = depthFormat;

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
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Failed to create graphics pipeline: " << e.what();
    throw ZException(fmt::format("Failed to create graphics pipeline: {}", e.what()));
  }
}

} // namespace nim
