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
  void create();

private:
  ZVulkanDevice& m_device;
  ZVulkanShader& m_shader;
  vk::PipelineVertexInputStateCreateInfo m_vertexInputInfo;
  vk::PrimitiveTopology m_topology;

  std::vector<vk::DescriptorSetLayout> m_descriptorSetLayouts;
  std::vector<vk::PushConstantRange> m_pushConstantRanges;

  std::optional<vk::raii::PipelineLayout> m_pipelineLayout;
  std::optional<vk::raii::Pipeline> m_pipeline;
};

} // namespace nim
