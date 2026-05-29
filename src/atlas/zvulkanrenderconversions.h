#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <QString>
#include <optional>
#include <string_view>
#include <vector>

namespace nim {
class ZVulkanDevice;
class ZVulkanBuffer;
class ZVulkanTexture;

enum class VulkanBlockIdCompactionMethod
{
  AppendStorageParallelFlush,
  AppendStorageParallelFlushGpuUnique,
  AppendSampledParallelFlush,
  DenseBitsetReadback,
  DenseBitsetFlagsReadback,
};

enum class VulkanBlockIdCompactionInputKind
{
  StorageImage,
  SampledImage,
};

[[nodiscard]] std::string_view blockIdCompactionMethodName(VulkanBlockIdCompactionMethod method);
[[nodiscard]] QString blockIdCompactionShaderFile(VulkanBlockIdCompactionMethod method);
[[nodiscard]] VulkanBlockIdCompactionInputKind blockIdCompactionInputKind(VulkanBlockIdCompactionMethod method);
[[nodiscard]] bool blockIdCompactionMethodUsesStorage(VulkanBlockIdCompactionMethod method);
[[nodiscard]] bool blockIdCompactionMethodUsesSampled(VulkanBlockIdCompactionMethod method);
[[nodiscard]] bool blockIdCompactionMethodIsDense(VulkanBlockIdCompactionMethod method);
} // namespace nim

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

// Converts renderer attachment handles back into Vulkan textures, enforcing that they
// originate from the active device. The usageDescription is surfaced in exception messages
// to aid debugging when stale handles are encountered during backend switches.
[[nodiscard]] ZVulkanTexture&
textureFromHandle(const AttachmentHandle& handle, ZVulkanDevice& device, std::string_view usageDescription);

[[nodiscard]] ZVulkanTexture&
textureFromHandle(const SampledImageHandle& handle, ZVulkanDevice& device, std::string_view usageDescription);

// Converts renderer buffer handles back into Vulkan buffers. For Vulkan, `id`
// is a reinterpret_cast<uint64_t>(ZVulkanBuffer*).
[[nodiscard]] ZVulkanBuffer&
bufferFromHandle(const BufferHandle& handle, ZVulkanDevice& device, std::string_view usageDescription);

} // namespace nim::vulkan
