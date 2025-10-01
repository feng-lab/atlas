#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <optional>
#include <vector>

namespace nim::vulkan {

struct AttachmentFormats
{
  std::vector<vk::Format> colorFormats;
  std::optional<vk::Format> depthFormat;
};

vk::Viewport toVkViewport(const ViewportDesc& viewport);

vk::Rect2D toVkScissor(const BackendPassDesc& pass);

vk::AttachmentLoadOp toVkLoadOp(LoadOp op);

vk::AttachmentStoreOp toVkStoreOp(StoreOp op);

AttachmentFormats extractAttachmentFormats(const RenderBatch& batch);

vk::PrimitiveTopology toVkPrimitive(PrimitiveTopology topology);

vk::CullModeFlags toVkCullMode(CullMode mode);

vk::FrontFace toVkFrontFace(FrontFace frontFace);

vk::PolygonMode toVkPolygonMode(FillMode mode);

vk::PipelineColorBlendAttachmentState toVkBlendAttachment(const BlendState& blend);

} // namespace nim::vulkan

