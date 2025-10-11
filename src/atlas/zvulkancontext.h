#pragma once

#include "zvulkan.h"
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
    return m_physicalDevices[0];
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

private:
  // Vulkan initialization steps
  void createInstance();
  void setupDebugMessenger();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createCommandPool();

  // Find queue families that support required operations
  QueueFamilyIndices findQueueFamilies(vk::raii::PhysicalDevice& physicalDevice) const;

  // Vulkan RAII objects
  std::optional<vk::raii::Context> m_context;
  std::optional<vk::raii::Instance> m_instance;
  std::optional<vk::raii::DebugUtilsMessengerEXT> m_debugMessenger;
  std::vector<vk::raii::PhysicalDevice> m_physicalDevices;
  std::optional<vk::raii::Device> m_device;
  std::optional<vk::raii::Queue> m_graphicsQueue;
  std::optional<vk::raii::Queue> m_presentQueue;
  std::optional<vk::raii::CommandPool> m_commandPool;

  QueueFamilyIndices m_queueFamilyIndices;
};

} // namespace nim
