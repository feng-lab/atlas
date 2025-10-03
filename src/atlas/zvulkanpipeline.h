#pragma once

#include "zvulkan.h"
#include <optional>
#include <vector>

namespace nim {

class ZVulkanDevice;
class ZVulkanShader;

class ZVulkanPipeline
{
public:
  ZVulkanPipeline(ZVulkanDevice& device,
                  ZVulkanShader& shader,
                  const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                  const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);
  ~ZVulkanPipeline();

  vk::Pipeline pipeline() const
  {
    return *m_pipeline;
  }
  vk::PipelineLayout pipelineLayout() const
  {
    return *m_pipelineLayout;
  }

  void setDescriptorSetLayouts(const std::vector<vk::DescriptorSetLayout>& layouts);
  void setPushConstantRanges(const std::vector<vk::PushConstantRange>& pushConstantRanges);
  void setPolygonMode(vk::PolygonMode mode);
  void setCullMode(vk::CullModeFlags mode);
  void setFrontFace(vk::FrontFace frontFace);
  void setDepthBias(bool enable, float constantFactor, float slopeFactor);
  void setLineWidth(float width);
  void setDepthWriteEnable(bool enable);
  void setDepthTestEnable(bool enable);
  void setDepthCompareOp(vk::CompareOp compareOp);
  void setColorBlendAttachment(const vk::PipelineColorBlendAttachmentState& attachment);
  void setColorBlendAttachments(std::vector<vk::PipelineColorBlendAttachmentState> attachments);
  void setAttachmentFormats(std::vector<vk::Format> colorFormats,
                            std::optional<vk::Format> depthFormat);
  void create();

private:
  ZVulkanDevice& m_device;
  ZVulkanShader& m_shader;
  vk::PipelineVertexInputStateCreateInfo m_vertexInputInfo;
  vk::PrimitiveTopology m_topology;

  std::vector<vk::DescriptorSetLayout> m_descriptorSetLayouts;
  std::vector<vk::PushConstantRange> m_pushConstantRanges;

  vk::PolygonMode m_polygonMode = vk::PolygonMode::eFill;
  vk::CullModeFlags m_cullMode = vk::CullModeFlagBits::eBack;
  vk::FrontFace m_frontFace = vk::FrontFace::eCounterClockwise;
  bool m_depthBiasEnable = false;
  float m_depthBiasConstant = 0.0f;
  float m_depthBiasSlope = 0.0f;
  float m_lineWidth = 1.0f;
  bool m_depthWriteEnable = true;
  bool m_depthTestEnable = true;
  vk::CompareOp m_depthCompareOp = vk::CompareOp::eLess;
  std::vector<vk::PipelineColorBlendAttachmentState> m_colorBlendAttachments{
    vk::PipelineColorBlendAttachmentState{.blendEnable = VK_TRUE,
                                          .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
                                          .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
                                          .colorBlendOp = vk::BlendOp::eAdd,
                                          .srcAlphaBlendFactor = vk::BlendFactor::eOne,
                                          .dstAlphaBlendFactor = vk::BlendFactor::eZero,
                                          .alphaBlendOp = vk::BlendOp::eAdd,
                                          .colorWriteMask = vk::ColorComponentFlagBits::eR |
                                                            vk::ColorComponentFlagBits::eG |
                                                            vk::ColorComponentFlagBits::eB |
                                                            vk::ColorComponentFlagBits::eA}};

  std::vector<vk::Format> m_colorAttachmentFormats{vk::Format::eR8G8B8A8Unorm};
  std::optional<vk::Format> m_depthAttachmentFormat = vk::Format::eD32Sfloat;

  std::optional<vk::raii::PipelineLayout> m_pipelineLayout;
  std::optional<vk::raii::Pipeline> m_pipeline;
};

} // namespace nim
