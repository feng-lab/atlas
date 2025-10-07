#pragma once

#include "zvulkan.h"
#include <vector>

namespace nim {

/**
 * @brief Pipeline state configuration for Vulkan renderers
 * Used to configure pipeline creation with different render states
 */
class ZVulkanPipelineState
{
public:
  ZVulkanPipelineState()
  {
    // Set default values
    cullMode = vk::CullModeFlagBits::eBack;
    frontFace = vk::FrontFace::eCounterClockwise;

    // Depth settings
    depthTestEnable = true;
    depthWriteEnable = true;
    depthCompareOp = vk::CompareOp::eLess;

    // Primitive topology
    topology = vk::PrimitiveTopology::eTriangleList;

    // Blending
    blendEnable = false;
    srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    colorBlendOp = vk::BlendOp::eAdd;
    srcAlphaBlendFactor = vk::BlendFactor::eOne;
    dstAlphaBlendFactor = vk::BlendFactor::eZero;
    alphaBlendOp = vk::BlendOp::eAdd;
  }

  // Rasterization state
  vk::CullModeFlagBits cullMode;
  vk::FrontFace frontFace;

  // Depth state
  bool depthTestEnable;
  bool depthWriteEnable;
  vk::CompareOp depthCompareOp;

  // Primitive assembly
  vk::PrimitiveTopology topology;

  // Color blend state
  bool blendEnable;
  vk::BlendFactor srcColorBlendFactor;
  vk::BlendFactor dstColorBlendFactor;
  vk::BlendOp colorBlendOp;
  vk::BlendFactor srcAlphaBlendFactor;
  vk::BlendFactor dstAlphaBlendFactor;
  vk::BlendOp alphaBlendOp;

  // Dynamic states
  std::vector<vk::DynamicState> dynamicStates;
};

} // namespace nim