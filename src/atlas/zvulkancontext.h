#pragma once

#include "zvulkan.h"
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
  struct BindlessSampledImageCapacities
  {
    uint32_t texture2D = 0;
    uint32_t texture2DArray = 0;
    uint32_t texture3D = 0;
    uint32_t uTexture2D = 0;
    uint32_t uTexture3D = 0;

    [[nodiscard]] uint32_t totalSampledImages() const
    {
      return texture2D + texture2DArray + texture3D + uTexture2D + uTexture3D;
    }

    [[nodiscard]] uint32_t fragmentVisibleSampledImages() const
    {
      return texture2D + texture2DArray + texture3D + uTexture3D;
    }

    [[nodiscard]] uint32_t computeVisibleSampledImages() const
    {
      return uTexture2D;
    }
  };

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

  // Bindless sampled-image table capacity policy:
  // - Requested capacities come from gflags (developer override).
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

  // Check if the device supports the required extensions
  bool checkDeviceExtensionSupport(vk::raii::PhysicalDevice& physicalDevice) const;

  void logGpuInfo() const;

  // Runtime device switching: select a new physical device by sorted index and
  // recreate the logical device, queues, and command pool. Returns true on
  // success. Callers must ensure no in-flight work depends on the prior device
  // (e.g., waitIdle and reset higher-level resources) before calling.
  bool setSelectedDeviceIndex(size_t index);

private:
  // Vulkan initialization steps
  void createInstance();
  void setupDebugMessenger();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createCommandPool();
  void computeBindlessSampledImageCapacities();

  // Find queue families that support required operations
  QueueFamilyIndices findQueueFamilies(vk::raii::PhysicalDevice& physicalDevice) const;

  // Vulkan RAII objects
  std::optional<vk::raii::Context> m_context;
  std::optional<vk::raii::Instance> m_instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> m_debugMessenger;
  std::vector<vk::raii::PhysicalDevice> m_physicalDevices;
  size_t m_selectedDeviceIndex = 0; // index into m_physicalDevices
  std::optional<vk::raii::Device> m_device;
  std::optional<vk::raii::Queue> m_graphicsQueue;
  std::optional<vk::raii::Queue> m_presentQueue;
  std::optional<vk::raii::CommandPool> m_commandPool;

  QueueFamilyIndices m_queueFamilyIndices;
  bool m_supportsInlineAndSecondaryDynamicRendering = false;
  bool m_supportsDescriptorIndexingSampledImageUpdateAfterBind = false;

  BindlessSampledImageCapacities m_requestedBindlessSampledImageCapacities{};
  BindlessSampledImageCapacities m_effectiveBindlessSampledImageCapacities{};
  bool m_bindlessSampledImageCapacitiesClamped = false;
};

} // namespace nim
