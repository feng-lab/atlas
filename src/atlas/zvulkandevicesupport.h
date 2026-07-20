#pragma once

#include "zvulkan.h"
#include "zvulkanbindings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nim {

// Immutable result of evaluating one physical device against Atlas' Vulkan
// contract. Selection and logical-device creation consume the same result so a
// device accepted during enumeration cannot fail later on a separately checked
// required feature or limit.
struct ZVulkanDeviceSupport
{
  enum class RejectionCode
  {
    ApiVersion,
    RequiredExtension,
    GraphicsQueue,
    ShaderClipDistance,
    ClipDistanceLimit,
    IndependentBlend,
    FillModeNonSolid,
    FragmentStoresAndAtomics,
    ColorAttachmentLimit,
    FragmentOutputAttachmentLimit,
    FragmentCombinedOutputLimit,
    ComputeWorkGroupLimits,
    PortabilityTriangleFans,
    DynamicRendering,
    Synchronization2,
    DescriptorIndexing,
    DescriptorCapacity,
    ImageFormatContract,
  };

  struct Rejection
  {
    RejectionCode code = RejectionCode::ApiVersion;
    std::string detail;
  };

  struct BindlessSampledImageCapacities
  {
    uint32_t texture2D = 0;
    uint32_t texture2DArray = 0;
    uint32_t texture3D = 0;
    uint32_t uTexture2D = 0;
    uint32_t uTexture3D = 0;

    [[nodiscard]] uint64_t totalSampledImages() const
    {
      return static_cast<uint64_t>(texture2D) + texture2DArray + texture3D + uTexture2D + uTexture3D;
    }

    [[nodiscard]] uint64_t fragmentVisibleSampledImages() const
    {
      return static_cast<uint64_t>(texture2D) + texture2DArray + texture3D + uTexture3D;
    }

    [[nodiscard]] uint64_t computeVisibleSampledImages() const
    {
      return uTexture2D;
    }
  };

  struct DescriptorLimits
  {
    uint32_t perStageSamplers = 0;
    uint32_t perSetSamplers = 0;
    uint32_t perStageSampledImages = 0;
    uint32_t perSetSampledImages = 0;
    uint32_t perStageResources = 0;
  };

  // Preference fields are kept separate from Vulkan handles so ordering can
  // be tested without a live Vulkan instance. UUID is only a deterministic
  // tie-break; power ranking remains type, device-local memory, then API.
  struct PhysicalDevicePreference
  {
    vk::PhysicalDeviceType deviceType = vk::PhysicalDeviceType::eOther;
    uint64_t deviceLocalMemoryBytes = 0u;
    uint32_t apiVersion = 0u;
    std::array<uint8_t, VK_UUID_SIZE> deviceUuid{};

    [[nodiscard]] static bool isPreferredBefore(const PhysicalDevicePreference& lhs,
                                                const PhysicalDevicePreference& rhs)
    {
      const int lhsTypeRank = typeRank(lhs.deviceType);
      const int rhsTypeRank = typeRank(rhs.deviceType);
      if (lhsTypeRank != rhsTypeRank) {
        return lhsTypeRank > rhsTypeRank;
      }
      if (lhs.deviceLocalMemoryBytes != rhs.deviceLocalMemoryBytes) {
        return lhs.deviceLocalMemoryBytes > rhs.deviceLocalMemoryBytes;
      }
      if (lhs.apiVersion != rhs.apiVersion) {
        return lhs.apiVersion > rhs.apiVersion;
      }
      return std::lexicographical_compare(lhs.deviceUuid.begin(),
                                          lhs.deviceUuid.end(),
                                          rhs.deviceUuid.begin(),
                                          rhs.deviceUuid.end());
    }

  private:
    [[nodiscard]] static int typeRank(vk::PhysicalDeviceType type)
    {
      switch (type) {
        case vk::PhysicalDeviceType::eDiscreteGpu:
          return 4;
        case vk::PhysicalDeviceType::eIntegratedGpu:
          return 3;
        case vk::PhysicalDeviceType::eVirtualGpu:
          return 2;
        case vk::PhysicalDeviceType::eCpu:
          return 1;
        default:
          return 0;
      }
    }
  };

  // Fixed descriptor/output resources that coexist with Atlas' bindless
  // sampled-image tables in the largest graphics and compute pipeline layouts.
  // Capacity evaluation reserves these before clamping the user-configurable
  // bindless tables against maxPerStageResources (or its update-after-bind
  // counterpart), preventing a per-type-valid layout from exceeding the
  // aggregate stage resource limit.
  struct ShaderResourcePolicy
  {
    static constexpr uint32_t kMaximumColorOutputs = 8u;
    static constexpr uint32_t kGraphicsFragmentUniformBuffers =
      vkbind::kLightingUniformBufferDescriptorCount + vkbind::kTransformsUniformBufferDescriptorCount;
    static constexpr uint32_t kGraphicsFragmentStorageBuffers = vkbind::kOITStorageBufferBindingCount;
    static constexpr uint32_t kGraphicsFragmentFixedResources =
      kGraphicsFragmentUniformBuffers + kGraphicsFragmentStorageBuffers + kMaximumColorOutputs;
    // Block-ID compaction is the largest compute layout that also exposes the
    // bindless compute sampled-image table: one SSBO plus one storage image.
    static constexpr uint32_t kComputeFixedResources = 2u;
    // ppll_resolve.frag statically uses four SSBOs and one color output.
    static constexpr uint32_t kPPLLFragmentCombinedOutputResources = 5u;
    // Block-ID raycasting writes the maximum eight color outputs. This is
    // larger than PPLL's combined SSBO/output footprint.
    static constexpr uint32_t kRequiredFragmentCombinedOutputResources =
      std::max(kMaximumColorOutputs, kPPLLFragmentCombinedOutputResources);
    static constexpr uint32_t kRequiredComputeWorkGroupInvocations = 256u;
    static constexpr uint32_t kRequiredComputeWorkGroupSizeX = 256u;
    static constexpr uint32_t kRequiredComputeWorkGroupSizeY = 16u;
    static constexpr uint32_t kMinimumComputeWorkGroupCountX = 1u;

    [[nodiscard]] static std::optional<uint32_t> fragmentBindlessBudget(uint32_t perStageResources)
    {
      if (perStageResources <= kGraphicsFragmentFixedResources) {
        return std::nullopt;
      }
      return perStageResources - kGraphicsFragmentFixedResources;
    }

    [[nodiscard]] static std::optional<uint32_t> computeBindlessBudget(uint32_t perStageResources)
    {
      if (perStageResources <= kComputeFixedResources) {
        return std::nullopt;
      }
      return perStageResources - kComputeFixedResources;
    }
  };

  // Keep descriptor-pool sizing and device-support accounting in one policy.
  // Ordinary transient and persistent pools use the legacy descriptor limit
  // class. Only the device-owned bindless pool uses update-after-bind, with one
  // bindless descriptor set for each frame slot.
  struct DescriptorPoolPolicy
  {
    static constexpr uint32_t kUniformBufferDescriptors = 1024u;
    static constexpr uint32_t kUniformBufferDynamicDescriptors = 2048u;
    static constexpr uint32_t kBindlessSamplerDescriptors = 3u;
    static constexpr uint32_t kStorageBufferDescriptors = 1024u;
    static constexpr uint32_t kStorageImageDescriptors = 2048u;
    static constexpr uint32_t kMaxSets = 4096u;

    // Return the maximum combined sampled-image capacity of one frame slot's
    // five bindless arrays after reserving the immutable sampler descriptors.
    [[nodiscard]] static std::optional<uint32_t>
    maxBindlessSampledImagesPerFrameSlot(uint32_t maxUpdateAfterBindDescriptorsInAllPools, uint32_t frameSlots)
    {
      if (frameSlots == 0u) {
        return std::nullopt;
      }
      const uint32_t descriptorsPerFrameSlot = maxUpdateAfterBindDescriptorsInAllPools / frameSlots;
      if (descriptorsPerFrameSlot < kBindlessSamplerDescriptors) {
        return std::nullopt;
      }
      return descriptorsPerFrameSlot - kBindlessSamplerDescriptors;
    }

    [[nodiscard]] static std::optional<uint64_t>
    requiredUpdateAfterBindDescriptors(const BindlessSampledImageCapacities& capacities, uint32_t frameSlots)
    {
      if (frameSlots == 0u) {
        return std::nullopt;
      }
      const uint64_t sampledImagesPerFrameSlot = capacities.totalSampledImages();
      if (sampledImagesPerFrameSlot > std::numeric_limits<uint64_t>::max() - kBindlessSamplerDescriptors) {
        return std::nullopt;
      }
      const uint64_t descriptorsPerFrameSlot = sampledImagesPerFrameSlot + kBindlessSamplerDescriptors;
      if (descriptorsPerFrameSlot > std::numeric_limits<uint64_t>::max() / frameSlots) {
        return std::nullopt;
      }
      return descriptorsPerFrameSlot * frameSlots;
    }
  };

  struct Selection
  {
    std::optional<size_t> index;
    std::string warning;
    std::string error;
  };

  std::optional<uint32_t> graphicsFamily;
  BindlessSampledImageCapacities requestedBindlessCapacities{};
  BindlessSampledImageCapacities effectiveBindlessCapacities{};
  DescriptorLimits descriptorLimits{};
  bool bindlessCapacitiesClamped = false;
  std::string bindlessClampReason;
  bool descriptorIndexingSampledImageUpdateAfterBind = false;
  uint32_t maxUpdateAfterBindDescriptorsInAllPools = 0u;
  uint64_t requiredUpdateAfterBindDescriptors = 0u;
  std::string updateAfterBindFallbackReason;
  bool memoryBudget = false;
  bool calibratedTimestamps = false;
  bool maintenance7 = false;
  bool nestedCommandBuffer = false;
  bool portabilityTriangleFans = false;
  uint32_t maxComputeWorkGroupCountX = 0u;
  std::vector<Rejection> rejections;

  [[nodiscard]] bool compatible() const
  {
    return rejections.empty();
  }

  [[nodiscard]] std::string rejectionSummary() const
  {
    std::string result;
    for (size_t i = 0; i < rejections.size(); ++i) {
      if (i != 0u) {
        result.append("; ");
      }
      result.append(rejections[i].detail);
    }
    return result;
  }

  // An explicit index is a best-effort preference. If it cannot be honored,
  // automatic selection chooses the first compatible device from the already
  // preference-sorted list and returns a warning for the caller to log.
  [[nodiscard]] static Selection select(std::span<const ZVulkanDeviceSupport> devices,
                                        std::optional<size_t> requestedIndex)
  {
    std::string preferenceRejection;
    if (requestedIndex.has_value()) {
      if (*requestedIndex >= devices.size()) {
        preferenceRejection = "Preferred Vulkan device index " + std::to_string(*requestedIndex) +
                              " is out of range for " + std::to_string(devices.size()) + " enumerated device(s)";
      } else if (!devices[*requestedIndex].compatible()) {
        preferenceRejection = "Preferred Vulkan device index " + std::to_string(*requestedIndex) +
                              " is incompatible: " + devices[*requestedIndex].rejectionSummary();
      } else {
        return {.index = *requestedIndex, .warning = {}, .error = {}};
      }
    }

    for (size_t i = 0; i < devices.size(); ++i) {
      if (devices[i].compatible()) {
        if (!preferenceRejection.empty()) {
          preferenceRejection.append("; using auto-selected compatible Vulkan device index ");
          preferenceRejection.append(std::to_string(i));
          preferenceRejection.append(" instead");
        }
        return {.index = i, .warning = std::move(preferenceRejection), .error = {}};
      }
    }

    if (!preferenceRejection.empty()) {
      preferenceRejection.append("; no compatible Vulkan fallback device is available");
    }
    std::string error = "No compatible Vulkan device was found";
    for (size_t i = 0; i < devices.size(); ++i) {
      error.append(i == 0u ? ": " : " | ");
      error.append("[");
      error.append(std::to_string(i));
      error.append("] ");
      error.append(devices[i].rejectionSummary());
    }
    return {.index = std::nullopt, .warning = std::move(preferenceRejection), .error = std::move(error)};
  }
};

} // namespace nim
