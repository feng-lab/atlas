#include "zvulkanrenderconversions.h"

#include "zexception.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"

#include <algorithm>

namespace nim::vulkan {
namespace {

vk::BlendFactor toVkBlendFactor(BlendFactor factor)
{
  switch (factor) {
    case BlendFactor::One:
      return vk::BlendFactor::eOne;
    case BlendFactor::Zero:
      return vk::BlendFactor::eZero;
    case BlendFactor::SrcAlpha:
      return vk::BlendFactor::eSrcAlpha;
    case BlendFactor::OneMinusSrcAlpha:
      return vk::BlendFactor::eOneMinusSrcAlpha;
    case BlendFactor::DstAlpha:
      return vk::BlendFactor::eDstAlpha;
    case BlendFactor::OneMinusDstAlpha:
      return vk::BlendFactor::eOneMinusDstAlpha;
  }
  return vk::BlendFactor::eOne;
}

vk::BlendOp toVkBlendOp(BlendOp op)
{
  switch (op) {
    case BlendOp::Add:
      return vk::BlendOp::eAdd;
    case BlendOp::Subtract:
      return vk::BlendOp::eSubtract;
    case BlendOp::ReverseSubtract:
      return vk::BlendOp::eReverseSubtract;
  }
  return vk::BlendOp::eAdd;
}

} // namespace

ZVulkanTexture&
textureFromHandle(const AttachmentHandle& handle, ZVulkanDevice& device, std::string_view usageDescription)
{
  if (!handle.valid() || handle.backend != RenderBackend::Vulkan) {
    throw ZException(fmt::format("{} requires a Vulkan attachment handle", usageDescription));
  }

  auto* texture = reinterpret_cast<ZVulkanTexture*>(handle.id);
  if (!texture) {
    throw ZException(fmt::format("{} provided a null Vulkan texture handle", usageDescription));
  }
  if (&texture->ownerDevice() != &device) {
    throw ZException(fmt::format("{} references a texture from a different Vulkan device", usageDescription));
  }
  return *texture;
}

ZVulkanTexture&
textureFromHandle(const SampledImageHandle& handle, ZVulkanDevice& device, std::string_view usageDescription)
{
  if (!handle.valid() || handle.backend != RenderBackend::Vulkan) {
    throw ZException(fmt::format("{} requires a Vulkan sampled image handle", usageDescription));
  }

  auto* texture = reinterpret_cast<ZVulkanTexture*>(handle.id);
  if (!texture) {
    throw ZException(fmt::format("{} provided a null Vulkan texture handle", usageDescription));
  }
  if (&texture->ownerDevice() != &device) {
    throw ZException(fmt::format("{} references a texture from a different Vulkan device", usageDescription));
  }
  return *texture;
}

vk::Viewport toVkViewport(const ViewportDesc& viewport)
{
  return vk::Viewport(viewport.origin.x,
                      viewport.origin.y,
                      std::max(0.0f, viewport.extent.x),
                      std::max(0.0f, viewport.extent.y),
                      viewport.minDepth,
                      viewport.maxDepth);
}

vk::Rect2D toVkScissor(const BackendPassDesc& pass)
{
  if (pass.enableScissor) {
    return vk::Rect2D({static_cast<int32_t>(pass.scissorRect.x), static_cast<int32_t>(pass.scissorRect.y)},
                      {static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.z)),
                       static_cast<uint32_t>(std::max(0.0f, pass.scissorRect.w))});
  }

  return vk::Rect2D({static_cast<int32_t>(pass.viewport.origin.x), static_cast<int32_t>(pass.viewport.origin.y)},
                    {static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.x)),
                     static_cast<uint32_t>(std::max(0.0f, pass.viewport.extent.y))});
}

vk::AttachmentLoadOp toVkLoadOp(LoadOp op)
{
  switch (op) {
    case LoadOp::Clear:
      return vk::AttachmentLoadOp::eClear;
    case LoadOp::Load:
      return vk::AttachmentLoadOp::eLoad;
    case LoadOp::DontCare:
    default:
      return vk::AttachmentLoadOp::eDontCare;
  }
}

vk::AttachmentStoreOp toVkStoreOp(StoreOp op)
{
  switch (op) {
    case StoreOp::Store:
      return vk::AttachmentStoreOp::eStore;
    case StoreOp::DontCare:
    default:
      return vk::AttachmentStoreOp::eDontCare;
  }
}

AttachmentFormats extractAttachmentFormats(const RenderBatch& batch)
{
  AttachmentFormats formats;
  for (const auto& attachment : batch.pass.colorAttachments) {
    if (attachment.handle.backend != RenderBackend::Vulkan || attachment.handle.id == 0u) {
      continue;
    }
    auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id);
    if (texture != nullptr) {
      formats.colorFormats.push_back(texture->format());
    }
  }

  if (batch.pass.depthAttachment) {
    const auto& attachment = *batch.pass.depthAttachment;
    if (attachment.handle.backend == RenderBackend::Vulkan && attachment.handle.id != 0u) {
      if (auto* texture = reinterpret_cast<ZVulkanTexture*>(attachment.handle.id)) {
        formats.depthFormat = texture->format();
      }
    }
  }

  return formats;
}

vk::PrimitiveTopology toVkPrimitive(PrimitiveTopology topology)
{
  switch (topology) {
    case PrimitiveTopology::PointList:
      return vk::PrimitiveTopology::ePointList;
    case PrimitiveTopology::LineList:
      return vk::PrimitiveTopology::eLineList;
    case PrimitiveTopology::LineStrip:
      return vk::PrimitiveTopology::eLineStrip;
    case PrimitiveTopology::TriangleStrip:
      return vk::PrimitiveTopology::eTriangleStrip;
    case PrimitiveTopology::TriangleList:
    default:
      return vk::PrimitiveTopology::eTriangleList;
  }
}

vk::CullModeFlags toVkCullMode(CullMode mode)
{
  switch (mode) {
    case CullMode::Front:
      return vk::CullModeFlagBits::eFront;
    case CullMode::Back:
      return vk::CullModeFlagBits::eBack;
    case CullMode::None:
    default:
      return vk::CullModeFlagBits::eNone;
  }
}

vk::FrontFace toVkFrontFace(FrontFace frontFace)
{
  switch (frontFace) {
    case FrontFace::Clockwise:
      return vk::FrontFace::eClockwise;
    case FrontFace::CounterClockwise:
    default:
      return vk::FrontFace::eCounterClockwise;
  }
}

vk::PolygonMode toVkPolygonMode(FillMode mode)
{
  switch (mode) {
    case FillMode::Wireframe:
      return vk::PolygonMode::eLine;
    case FillMode::Solid:
    default:
      return vk::PolygonMode::eFill;
  }
}

vk::PipelineColorBlendAttachmentState toVkBlendAttachment(const BlendState& blend)
{
  vk::PipelineColorBlendAttachmentState attachment{};
  attachment.blendEnable = static_cast<vk::Bool32>(blend.enabled);
  attachment.srcColorBlendFactor = toVkBlendFactor(blend.srcColor);
  attachment.dstColorBlendFactor = toVkBlendFactor(blend.dstColor);
  attachment.colorBlendOp = toVkBlendOp(blend.colorOp);
  attachment.srcAlphaBlendFactor = toVkBlendFactor(blend.srcAlpha);
  attachment.dstAlphaBlendFactor = toVkBlendFactor(blend.dstAlpha);
  attachment.alphaBlendOp = toVkBlendOp(blend.alphaOp);
  attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  return attachment;
}

} // namespace nim::vulkan
