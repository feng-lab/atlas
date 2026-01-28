#pragma once

#include "z3drendercommands.h"
#include "zlog.h"
#include "zvulkan.h"

namespace nim::vulkan {

struct ExternalImageUseVulkanInfo
{
  vk::ImageLayout layout{vk::ImageLayout::eUndefined};
  // Aspect mask used for layout transitions/barriers. Empty means "use the
  // texture's full aspect mask" (color images), but depth/stencil reads must be
  // explicit to avoid format-based inference.
  vk::ImageAspectFlags transitionAspect{};
  // Aspect override used for descriptor writes (when binding a depth/stencil
  // image view as a sampled input).
  vk::ImageAspectFlags descriptorAspect{};
  bool updateDescriptorLayout{false};
};

[[nodiscard]] inline ExternalImageUseVulkanInfo resolveExternalImageUse(ExternalImageUseKind kind,
                                                                        ExternalImageAspectHint aspectHint)
{
  ExternalImageUseVulkanInfo result{};

  switch (kind) {
    case ExternalImageUseKind::SampledRead: {
      result.updateDescriptorLayout = true;
      switch (aspectHint) {
        case ExternalImageAspectHint::Unspecified:
          CHECK(false) << "ExternalImageUseDesc missing aspectHint for sampled read (explicit metadata required)";
          result.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
          result.transitionAspect = {};
          result.descriptorAspect = {};
          return result;
        case ExternalImageAspectHint::Color:
          result.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
          result.transitionAspect = {};
          result.descriptorAspect = {};
          return result;
        case ExternalImageAspectHint::Depth:
          result.layout = vk::ImageLayout::eDepthReadOnlyOptimal;
          result.transitionAspect = vk::ImageAspectFlagBits::eDepth;
          result.descriptorAspect = vk::ImageAspectFlagBits::eDepth;
          return result;
        case ExternalImageAspectHint::Stencil:
          result.layout = vk::ImageLayout::eStencilReadOnlyOptimal;
          result.transitionAspect = vk::ImageAspectFlagBits::eStencil;
          result.descriptorAspect = vk::ImageAspectFlagBits::eStencil;
          return result;
      }
      CHECK(false) << "Unhandled ExternalImageAspectHint";
      return result;
    }
    case ExternalImageUseKind::StorageRead:
    case ExternalImageUseKind::StorageWrite:
    case ExternalImageUseKind::StorageReadWrite:
      result.layout = vk::ImageLayout::eGeneral;
      result.updateDescriptorLayout = true;
      return result;
    case ExternalImageUseKind::TransferSrc:
      result.layout = vk::ImageLayout::eTransferSrcOptimal;
      return result;
    case ExternalImageUseKind::TransferDst:
      result.layout = vk::ImageLayout::eTransferDstOptimal;
      return result;
    case ExternalImageUseKind::General:
      result.layout = vk::ImageLayout::eGeneral;
      result.updateDescriptorLayout = true;
      return result;
  }

  CHECK(false) << "Unhandled ExternalImageUseKind";
  return result;
}

struct ExternalBufferUseVulkanInfo
{
  vk::PipelineStageFlags2 stage{};
  vk::AccessFlags2 access{};
};

[[nodiscard]] inline vk::ImageLayout depthAttachmentLayoutForAspect(vk::ImageAspectFlags aspect)
{
  CHECK((aspect & (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil)) != vk::ImageAspectFlags{})
    << "Depth attachment layout requested for non-depth/stencil aspect mask";
  if (aspect & vk::ImageAspectFlagBits::eStencil) {
    return vk::ImageLayout::eDepthStencilAttachmentOptimal;
  }
  return vk::ImageLayout::eDepthAttachmentOptimal;
}

[[nodiscard]] inline ExternalBufferUseVulkanInfo resolveExternalBufferUse(ExternalBufferUseKind kind,
                                                                          vk::BufferUsageFlags usage)
{
  ExternalBufferUseVulkanInfo result{};

  switch (kind) {
    case ExternalBufferUseKind::Unspecified:
      CHECK(false) << "ExternalBufferUseDesc missing kind (explicit metadata required)";
      result.stage = vk::PipelineStageFlagBits2::eAllCommands;
      result.access = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
      return result;
    case ExternalBufferUseKind::UniformRead:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eUniformBuffer))
        << "ExternalBufferUseKind::UniformRead requires VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT";
      result.stage = vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
      result.access = vk::AccessFlagBits2::eUniformRead;
      return result;
    case ExternalBufferUseKind::StorageRead:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eStorageBuffer))
        << "ExternalBufferUseKind::StorageRead requires VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
      result.stage = vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
      result.access = vk::AccessFlagBits2::eShaderRead;
      return result;
    case ExternalBufferUseKind::StorageWrite:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eStorageBuffer))
        << "ExternalBufferUseKind::StorageWrite requires VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
      result.stage = vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
      result.access = vk::AccessFlagBits2::eShaderWrite;
      return result;
    case ExternalBufferUseKind::StorageReadWrite:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eStorageBuffer))
        << "ExternalBufferUseKind::StorageReadWrite requires VK_BUFFER_USAGE_STORAGE_BUFFER_BIT";
      result.stage = vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
      result.access = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
      return result;
    case ExternalBufferUseKind::TransferSrc:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eTransferSrc))
        << "ExternalBufferUseKind::TransferSrc requires VK_BUFFER_USAGE_TRANSFER_SRC_BIT";
      result.stage = vk::PipelineStageFlagBits2::eTransfer;
      result.access = vk::AccessFlagBits2::eTransferRead;
      return result;
    case ExternalBufferUseKind::TransferDst:
      CHECK(static_cast<bool>(usage & vk::BufferUsageFlagBits::eTransferDst))
        << "ExternalBufferUseKind::TransferDst requires VK_BUFFER_USAGE_TRANSFER_DST_BIT";
      result.stage = vk::PipelineStageFlagBits2::eTransfer;
      result.access = vk::AccessFlagBits2::eTransferWrite;
      return result;
    case ExternalBufferUseKind::General:
      result.stage = vk::PipelineStageFlagBits2::eAllCommands;
      result.access = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
      return result;
  }

  CHECK(false) << "Unhandled ExternalBufferUseKind";
  return result;
}

} // namespace nim::vulkan
