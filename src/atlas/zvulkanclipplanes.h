#pragma once

#include "z3drendercommands.h"
#include "zvulkanuniforms.h"
#include "zlog.h"

namespace nim {
namespace vulkan {

inline void applyBatchClipPlanesToTransforms(const RenderBatch& batch, TransformsUBOStd140& transforms)
{
  static_assert(kRenderBatchMaxClipPlanes == kVulkanMaxClipPlanes, "Clip plane limits must match across layers");

  CHECK(batch.clipPlanes.captured) << "Vulkan render batch missing captured clip plane state";
  const size_t planeCount = static_cast<size_t>(batch.clipPlanes.planeCount);
  CHECK(planeCount <= kVulkanMaxClipPlanes)
    << "Vulkan clip plane overflow: planes=" << planeCount << " max=" << kVulkanMaxClipPlanes;

  transforms.clip_params = glm::ivec4(batch.clipPlanes.enabled ? 1 : 0, static_cast<int>(planeCount), 0, 0);
  for (size_t i = 0; i < planeCount; ++i) {
    transforms.clip_planes[i] = batch.clipPlanes.planes[i];
  }
}

inline void applyBatchClipPlanesToTransforms(const RenderBatch& batch, ObjectTransformsUBOStd140& transforms)
{
  static_assert(kRenderBatchMaxClipPlanes == kVulkanMaxClipPlanes, "Clip plane limits must match across layers");

  CHECK(batch.clipPlanes.captured) << "Vulkan render batch missing captured clip plane state";
  const size_t planeCount = static_cast<size_t>(batch.clipPlanes.planeCount);
  CHECK(planeCount <= kVulkanMaxClipPlanes)
    << "Vulkan clip plane overflow: planes=" << planeCount << " max=" << kVulkanMaxClipPlanes;

  transforms.clip_params = glm::ivec4(batch.clipPlanes.enabled ? 1 : 0, static_cast<int>(planeCount), 0, 0);
  for (size_t i = 0; i < planeCount; ++i) {
    transforms.clip_planes[i] = batch.clipPlanes.planes[i];
  }
}

} // namespace vulkan
} // namespace nim
