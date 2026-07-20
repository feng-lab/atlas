#pragma once

#include <cstdint>

namespace nim {

// Resource families that must be ready before a Vulkan submission starts
// recording. Keeping this small mask independent of the renderer backend lets
// linear-script headers declare requirements without importing backend state.
enum class ZVulkanSubmissionRequirement : uint32_t
{
  None = 0u,
  DDPGating = 1u << 0u,
};

using ZVulkanSubmissionRequirements = uint32_t;

[[nodiscard]] constexpr ZVulkanSubmissionRequirements
vulkanSubmissionRequirementMask(ZVulkanSubmissionRequirement requirement)
{
  return static_cast<ZVulkanSubmissionRequirements>(requirement);
}

} // namespace nim
