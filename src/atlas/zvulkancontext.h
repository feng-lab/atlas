#pragma once

#include "zvulkan.h"
#include "zvulkandevicesupport.h"
#include "zlog.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace nim {

class ZVulkanDevice;

/**
 * @brief Main class for Vulkan device management and resource creation
 * Similar to Z3DContext in the OpenGL implementation
 */
class ZVulkanContext
{
public:
  using BindlessSampledImageCapacities = ZVulkanDeviceSupport::BindlessSampledImageCapacities;

  ZVulkanContext();
  ~ZVulkanContext();

  vk::raii::Context& context()
  {
    return *m_context;
  }

  // Returns the Vulkan RAII instance
  vk::raii::Instance& instance()
  {
    return *m_instance;
  }

  // Returns the currently selected physical device
  vk::raii::PhysicalDevice& physicalDevice()
  {
    return m_physicalDevices[m_selectedDeviceIndex];
  }

  // Returns the Nth enumerated physical device (sorted by preference).
  // Caller must ensure index < deviceCount().
  vk::raii::PhysicalDevice& physicalDevice(size_t index)
  {
    return m_physicalDevices[index];
  }

  size_t deviceCount() const
  {
    return m_physicalDevices.size();
  }

  size_t selectedDeviceIndex() const
  {
    return m_selectedDeviceIndex;
  }

  // Immutable topology captured before physical-device evaluation. Descriptor
  // budgeting, bindless slots, and every wrapper use this one value.
  [[nodiscard]] uint32_t frameSlotCount() const
  {
    return m_frameSlotCount;
  }

  // Returns the logical device
  vk::raii::Device& device()
  {
    return *m_device;
  }

  // Returns graphics queue
  vk::raii::Queue& graphicsQueue()
  {
    return *m_graphicsQueue;
  }

  // Returns presentation queue
  vk::raii::Queue& presentQueue()
  {
    return *m_presentQueue;
  }

  // Return command pool
  vk::CommandPool commandPool() const
  {
    return *m_commandPool;
  }

  // True when the device was created with either VK_KHR_maintenance7
  // (maintenance7 feature) or VK_EXT_nested_command_buffer (nestedCommandBuffer
  // feature) enabled. This gates using VkRenderingInfo::flags with both inline
  // and secondary-contents bits inside vkCmdBeginRendering.
  [[nodiscard]] bool supportsInlineAndSecondaryDynamicRendering() const
  {
    return m_supportsInlineAndSecondaryDynamicRendering;
  }

  // True when the logical device enabled descriptor indexing update-after-bind
  // for sampled images. Atlas does not rely on
  // updating descriptors while in-flight, but enabling update-after-bind lets
  // some drivers (notably MoltenVK) account large bindless descriptor arrays
  // against the descriptor indexing limits rather than the much smaller legacy
  // per-stage sampler limits.
  [[nodiscard]] bool supportsDescriptorIndexingSampledImageUpdateAfterBind() const
  {
    return m_supportsDescriptorIndexingSampledImageUpdateAfterBind;
  }

  [[nodiscard]] bool supportsCalibratedTimestamps() const
  {
    return m_supportsCalibratedTimestamps;
  }

  [[nodiscard]] bool supportsMemoryBudget() const
  {
    return selectedDeviceSupport().memoryBudget;
  }

  [[nodiscard]] const ZVulkanDeviceSupport& selectedDeviceSupport() const
  {
    CHECK(m_selectedDeviceIndex < m_deviceSupports.size());
    return m_deviceSupports[m_selectedDeviceIndex];
  }

  // Bindless sampled-image table capacity policy:
  // - Requested capacities come from command-line flags (developer override).
  // - Effective capacities are clamped once per logical device creation to fit
  //   the selected physical device limits (legacy or update-after-bind) and are
  //   treated as immutable for the device lifetime (pipeline layouts depend on them).
  [[nodiscard]] const BindlessSampledImageCapacities& requestedBindlessSampledImageCapacities() const
  {
    return m_requestedBindlessSampledImageCapacities;
  }

  [[nodiscard]] const BindlessSampledImageCapacities& effectiveBindlessSampledImageCapacities() const
  {
    return m_effectiveBindlessSampledImageCapacities;
  }

  [[nodiscard]] bool bindlessSampledImageCapacitiesClamped() const
  {
    return m_bindlessSampledImageCapacitiesClamped;
  }

  // Queue family indices for the selected physical device
  struct QueueFamilyIndices
  {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const
    {
      return graphicsFamily.has_value() && presentFamily.has_value();
    }
  };

  QueueFamilyIndices queueFamilyIndices() const
  {
    return m_queueFamilyIndices;
  }

  // Create a ZVulkanDevice instance from this context
  std::unique_ptr<ZVulkanDevice> createDevice();
  // Internal lifetime callbacks used by ZVulkanDevice. A context owns one
  // logical vk::Device and permits exactly one live wrapper/accounting domain,
  // including for direct wrapper construction outside createDevice().
  void notifyDeviceWrapperCreated(const ZVulkanDevice* device);
  void notifyDeviceWrapperDestroyed(const ZVulkanDevice* device);

  void logGpuInfo() const;

private:
  // Vulkan initialization steps
  void createInstance();
  void setupDebugMessenger();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createCommandPool();
  void computeBindlessSampledImageCapacities();
  ZVulkanDeviceSupport evaluateDeviceSupport(vk::raii::PhysicalDevice& physicalDevice) const;

  // Find queue families that support required operations
  QueueFamilyIndices findQueueFamilies(vk::raii::PhysicalDevice& physicalDevice) const;

  // Vulkan RAII objects
  const uint32_t m_frameSlotCount;
  std::optional<vk::raii::Context> m_context;
  std::optional<vk::raii::Instance> m_instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> m_debugMessenger;
  std::vector<vk::raii::PhysicalDevice> m_physicalDevices;
  std::vector<ZVulkanDeviceSupport> m_deviceSupports;
  size_t m_selectedDeviceIndex = 0; // index into m_physicalDevices
  std::optional<vk::raii::Device> m_device;
  std::optional<vk::raii::Queue> m_graphicsQueue;
  std::optional<vk::raii::Queue> m_presentQueue;
  std::optional<vk::raii::CommandPool> m_commandPool;

  QueueFamilyIndices m_queueFamilyIndices;
  bool m_supportsInlineAndSecondaryDynamicRendering = false;
  bool m_supportsDescriptorIndexingSampledImageUpdateAfterBind = false;
  bool m_supportsCalibratedTimestamps = false;

  BindlessSampledImageCapacities m_requestedBindlessSampledImageCapacities{};
  BindlessSampledImageCapacities m_effectiveBindlessSampledImageCapacities{};
  bool m_bindlessSampledImageCapacitiesClamped = false;
  const ZVulkanDevice* m_liveDeviceWrapper = nullptr;
};

} // namespace nim
