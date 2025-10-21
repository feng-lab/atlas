#pragma once

#include "zvulkan.h"
#include "zlog.h"

namespace nim {

struct LayoutState
{
  vk::AccessFlags2 access;
  vk::PipelineStageFlags2 stage;
};

inline LayoutState layoutStateFor(vk::ImageLayout layout)
{
  using vk::AccessFlagBits2;
  using vk::PipelineStageFlagBits2;

  switch (layout) {
    case vk::ImageLayout::eUndefined:
      return {vk::AccessFlags2{}, PipelineStageFlagBits2::eTopOfPipe};
    case vk::ImageLayout::eGeneral:
      return {AccessFlagBits2::eShaderRead | AccessFlagBits2::eShaderWrite, PipelineStageFlagBits2::eAllCommands};
    case vk::ImageLayout::eColorAttachmentOptimal:
      return {AccessFlagBits2::eColorAttachmentRead | AccessFlagBits2::eColorAttachmentWrite,
              PipelineStageFlagBits2::eColorAttachmentOutput};
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
    case vk::ImageLayout::eDepthAttachmentOptimal:
    case vk::ImageLayout::eStencilAttachmentOptimal:
      return {AccessFlagBits2::eDepthStencilAttachmentRead | AccessFlagBits2::eDepthStencilAttachmentWrite,
              PipelineStageFlagBits2::eEarlyFragmentTests | PipelineStageFlagBits2::eLateFragmentTests};
    case vk::ImageLayout::eDepthReadOnlyOptimal:
    case vk::ImageLayout::eStencilReadOnlyOptimal:
    case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
    case vk::ImageLayout::eDepthAttachmentStencilReadOnlyOptimal:
    case vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal:
      return {AccessFlagBits2::eDepthStencilAttachmentRead,
              PipelineStageFlagBits2::eEarlyFragmentTests | PipelineStageFlagBits2::eLateFragmentTests};
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      return {AccessFlagBits2::eShaderRead,
              PipelineStageFlagBits2::eAllGraphics | PipelineStageFlagBits2::eComputeShader |
                PipelineStageFlagBits2::eRayTracingShaderKHR};
    case vk::ImageLayout::eTransferDstOptimal:
      return {AccessFlagBits2::eTransferWrite, PipelineStageFlagBits2::eTransfer};
    case vk::ImageLayout::eTransferSrcOptimal:
      return {AccessFlagBits2::eTransferRead, PipelineStageFlagBits2::eTransfer};
    case vk::ImageLayout::eAttachmentOptimalKHR:
      return {AccessFlagBits2::eColorAttachmentRead | AccessFlagBits2::eColorAttachmentWrite |
                AccessFlagBits2::eDepthStencilAttachmentRead | AccessFlagBits2::eDepthStencilAttachmentWrite,
              PipelineStageFlagBits2::eAllGraphics};
    case vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT:
      return {AccessFlagBits2::eColorAttachmentRead | AccessFlagBits2::eColorAttachmentWrite |
                AccessFlagBits2::eDepthStencilAttachmentRead | AccessFlagBits2::eDepthStencilAttachmentWrite |
                AccessFlagBits2::eShaderRead | AccessFlagBits2::eShaderWrite,
              PipelineStageFlagBits2::eAllGraphics | PipelineStageFlagBits2::eComputeShader};
    case vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR:
      return {AccessFlagBits2::eFragmentShadingRateAttachmentReadKHR,
              PipelineStageFlagBits2::eFragmentShadingRateAttachmentKHR};
    case vk::ImageLayout::eFragmentDensityMapOptimalEXT:
      return {AccessFlagBits2::eFragmentDensityMapReadEXT, PipelineStageFlagBits2::eFragmentDensityProcessEXT};
    case vk::ImageLayout::ePreinitialized:
      return {AccessFlagBits2::eHostWrite, PipelineStageFlagBits2::eHost};
    case vk::ImageLayout::ePresentSrcKHR:
    case vk::ImageLayout::eSharedPresentKHR:
      return {vk::AccessFlags2{}, PipelineStageFlagBits2::eBottomOfPipe};
    default:
      CHECK(false) << "Unsupported image layout in layoutStateFor: " << enumOrUnderlying(layout, 16);
      return {vk::AccessFlags2{}, PipelineStageFlagBits2::eAllCommands};
  }
}

} // namespace nim
