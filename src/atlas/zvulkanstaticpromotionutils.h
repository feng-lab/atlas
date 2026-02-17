#pragma once

#include "z3drenderervulkanbackend.h"

#include <initializer_list>
#include <vector>

namespace nim::vulkan {

// Small helpers to keep Vulkan "upload slice -> device-local static" promotion
// call sites compact and consistent across pipeline contexts.

struct StaticPromotionPlan
{
  Z3DRendererVulkanBackend::StaticSlice* dst = nullptr;
  // May be null when bytes == 0 (no copy).
  const Z3DRendererVulkanBackend::UploadSlice* src = nullptr;
  size_t bytes = 0;
  size_t alignment = 16;
  bool isIndexBuffer = false;
};

// Allocate all requested static slices and schedule the corresponding upload->static
// copies. If any allocation fails, previously allocated slices are released to
// avoid leaking static-arena virtual allocations.
//
// Notes:
// - Requests with bytes == 0 are treated as success and reset dst to empty.
// - Copies are scheduled only after all allocations succeed.
// - outBytesStaged, when provided, reports the total bytes scheduled for copy.
inline bool allocateAndScheduleStaticCopies(Z3DRendererVulkanBackend& backend,
                                            std::initializer_list<StaticPromotionPlan> plans,
                                            /*nullable*/ size_t* outBytesStaged = nullptr)
{
  size_t stagedBytes = 0;
  std::vector<Z3DRendererVulkanBackend::StaticSlice*> allocated;
  allocated.reserve(plans.size());

  for (const auto& plan : plans) {
    CHECK(plan.dst != nullptr) << "StaticPromotionPlan requires dst";
    if (plan.bytes == 0) {
      *plan.dst = {};
      continue;
    }
    CHECK(plan.src != nullptr) << "StaticPromotionPlan with bytes>0 requires src";

    Z3DRendererVulkanBackend::StaticSlice slice = plan.isIndexBuffer
                                                    ? backend.allocateStaticIB(plan.bytes, plan.alignment)
                                                    : backend.allocateStaticVB(plan.bytes, plan.alignment);
    if (!slice) {
      for (auto* s : allocated) {
        backend.releaseStaticSlice(*s);
      }
      if (outBytesStaged) {
        *outBytesStaged = 0;
      }
      return false;
    }
    *plan.dst = slice;
    allocated.push_back(plan.dst);
    stagedBytes += plan.bytes;
  }

  for (const auto& plan : plans) {
    if (plan.bytes == 0) {
      continue;
    }
    CHECK(plan.dst != nullptr);
    CHECK(plan.src != nullptr);
    backend.scheduleStaticCopy(plan.dst->buffer, plan.dst->offset, *plan.src, plan.isIndexBuffer);
  }

  if (outBytesStaged) {
    *outBytesStaged = stagedBytes;
  }
  return true;
}

inline void releaseStaticSlices(Z3DRendererVulkanBackend& backend,
                                std::initializer_list<Z3DRendererVulkanBackend::StaticSlice*> slices)
{
  for (auto* slice : slices) {
    CHECK(slice != nullptr) << "releaseStaticSlices called with null slice pointer";
    backend.releaseStaticSlice(*slice);
  }
}

inline void
pinStaticSlicesForActiveSubmission(Z3DRendererVulkanBackend& backend,
                                   std::initializer_list<const Z3DRendererVulkanBackend::StaticSlice*> slices)
{
  for (const auto* slice : slices) {
    CHECK(slice != nullptr) << "pinStaticSlicesForActiveSubmission called with null slice pointer";
    backend.pinStaticSliceForActiveSubmission(*slice);
  }
}

} // namespace nim::vulkan
