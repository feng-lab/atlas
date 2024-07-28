#include "zvulkan.h"

#include "zlog.h"

#define ATLAS_USE_VULKAN_DEBUG

namespace nim {

std::string uuidToString(const uint8_t uuid[VK_UUID_SIZE])
{
  return fmt::format(
    "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
    uuid[0],
    uuid[1],
    uuid[2],
    uuid[3],
    uuid[4],
    uuid[5],
    uuid[6],
    uuid[7],
    uuid[8],
    uuid[9],
    uuid[10],
    uuid[11],
    uuid[12],
    uuid[13],
    uuid[14],
    uuid[15]);
}

std::string versionToString(uint32_t version)
{
  return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
}

// Function to check if a layer is available
bool isLayerAvailable(const std::vector<vk::LayerProperties>& availableLayers, const char* layerName)
{
  return std::find_if(availableLayers.begin(),
                      availableLayers.end(),
                      [layerName](const vk::LayerProperties& layerProperties) {
                        return strcmp(layerName, layerProperties.layerName) == 0;
                      }) != availableLayers.end();
}

// Function to check if an extension is available
bool isExtensionAvailable(const std::vector<vk::ExtensionProperties>& availableExtensions, const char* extensionName)
{
  return std::find_if(availableExtensions.begin(),
                      availableExtensions.end(),
                      [extensionName](const vk::ExtensionProperties& extensionProperties) {
                        return strcmp(extensionName, extensionProperties.extensionName) == 0;
                      }) != availableExtensions.end();
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                           VkDebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                           void* /*pUserData*/)
{
  auto logLevel = google::GLOG_INFO;
  if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    logLevel = google::GLOG_WARNING;
  } else if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    logLevel = google::GLOG_ERROR;
  }

  std::string message = pCallbackData->pMessage;

  if (pCallbackData->queueLabelCount > 0) {
    message += "\n\tQueue Labels:";
    for (uint32_t i = 0; i < pCallbackData->queueLabelCount; i++) {
      message += fmt::format("\n\t\tlabelName = <{}>", pCallbackData->pQueueLabels[i].pLabelName);
    }
  }

  if (pCallbackData->cmdBufLabelCount > 0) {
    message += "\n\tCommandBuffer Labels:";
    for (uint32_t i = 0; i < pCallbackData->cmdBufLabelCount; i++) {
      message += fmt::format("\n\t\tlabelName = <{}>", pCallbackData->pCmdBufLabels[i].pLabelName);
    }
  }

  google::LogMessage(__FILE__, __LINE__, logLevel).stream() << message;

  return VK_FALSE;
}

// Create Vulkan instance
vk::raii::Instance createVulkanInstance(vk::raii::Context& context, uint32_t desiredVersion)
{
  vk::ApplicationInfo appInfo{.pApplicationName = "Atlas",
                              .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                              .pEngineName = "Atlas",
                              .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                              .apiVersion = desiredVersion};

  std::vector<const char*> layers;
  std::vector<const char*> extensions;
  vk::InstanceCreateFlags instanceFlags;

  // Get available layers and extensions
  std::vector<vk::LayerProperties> availableLayers = context.enumerateInstanceLayerProperties();
  LOG(INFO) << fmt::format("Available Instance Layers ({})", availableLayers.size());
  for (const auto& layer : availableLayers) {
    LOG(INFO) << fmt::format("  Layer Name: {} (Description: {}, Spec Version: {}.{}.{}, Implementation Version: {})",
                             layer.layerName,
                             layer.description,
                             VK_VERSION_MAJOR(layer.specVersion),
                             VK_VERSION_MINOR(layer.specVersion),
                             VK_VERSION_PATCH(layer.specVersion),
                             layer.implementationVersion);
  }

  std::vector<vk::ExtensionProperties> availableExtensions = context.enumerateInstanceExtensionProperties();
  LOG(INFO) << fmt::format("Available Instance Extensions ({})", availableExtensions.size());
  for (const auto& extension : availableExtensions) {
    LOG(INFO) << fmt::format("  Extension Name: {} (Spec Version: {})", extension.extensionName, extension.specVersion);
  }

  // // enumerate extensions for each layer
  // for (const auto& layer : context.enumerateInstanceLayerProperties()) {
  //   try {
  //     auto layerExtensions = context.enumerateInstanceExtensionProperties(std::string(layer.layerName));
  //     LOG(INFO) << fmt::format("Extensions for Layer '{}' ({})", layer.layerName, layerExtensions.size());
  //     for (const auto& extension : layerExtensions) {
  //       LOG(INFO) << fmt::format("  Extension Name: {} (Spec Version: {})",
  //                                extension.extensionName,
  //                                extension.specVersion);
  //     }
  //   }
  //   catch (const std::exception& e) {
  //     LOG(ERROR) << fmt::format("Failed to enumerate extensions for layer '{}': {}", layer.layerName, e.what());
  //   }
  // }

#ifdef ATLAS_USE_VULKAN_DEBUG
  // Enable validation layers in debug mode
  const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
  if (isLayerAvailable(availableLayers, validationLayerName)) {
    layers.push_back(validationLayerName);
  } else {
    LOG(WARNING) << "Validation layer not available, continuing without it";
  }

  // Enable debug utils extension
  if (isExtensionAvailable(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  } else {
    LOG(WARNING) << "Debug extension " << VK_EXT_DEBUG_UTILS_EXTENSION_NAME << " not available";
  }
#endif

#ifdef __APPLE__
  // Enable portability enumeration on macOS
  if (isExtensionAvailable(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    instanceFlags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
  } else {
    LOG(WARNING) << "Portability enumeration extension not available on macOS";
  }
#endif

  // Create the Vulkan instance
  vk::InstanceCreateInfo createInfo{.flags = instanceFlags,
                                    .pApplicationInfo = &appInfo,
                                    .enabledLayerCount = static_cast<uint32_t>(layers.size()),
                                    .ppEnabledLayerNames = layers.data(),
                                    .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
                                    .ppEnabledExtensionNames = extensions.data()};

  vk::raii::Instance instance(context, createInfo);
  LOG(INFO) << "Vulkan instance created successfully";

  // Log enabled layers and extensions
  LOG(INFO) << "Enabled Layers:";
  for (const auto& layer : layers) {
    LOG(INFO) << "  " << layer;
  }
  LOG(INFO) << "Enabled Extensions:";
  for (const auto& extension : extensions) {
    LOG(INFO) << "  " << extension;
  }

  return instance;
}

void initVulkan()
{
  try {
    // instantiate a vk::raii::Context
    vk::raii::Context context;

    // Determine what API version is available
    uint32_t apiVersion = context.enumerateInstanceVersion();

    LOG(INFO) << fmt::format("Available Vulkan API version: {}.{}.{}",
                             VK_VERSION_MAJOR(apiVersion),
                             VK_VERSION_MINOR(apiVersion),
                             VK_VERSION_PATCH(apiVersion));

    // Set the desired version we want
    constexpr uint16_t desiredMajorVersion = 1;
    constexpr uint16_t desiredMinorVersion = 2;
    constexpr uint32_t desiredVersion = VK_MAKE_VERSION(desiredMajorVersion, desiredMinorVersion, 0);
    std::string desiredVersionString = fmt::format("{}.{}.{}", desiredMajorVersion, desiredMinorVersion, 0);

    auto instance = createVulkanInstance(context, desiredVersion);

#ifdef ATLAS_USE_VULKAN_DEBUG
    // Set up debug messenger
    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{
      .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                         vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                         vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
      .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                     vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                     vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
      .pfnUserCallback = debugUtilsMessengerCallback};

    vk::raii::DebugUtilsMessengerEXT debugUtilsMessenger(nullptr);
    try {
      debugUtilsMessenger = vk::raii::DebugUtilsMessengerEXT(instance, debugCreateInfo);
      LOG(INFO) << "Debug messenger created successfully";
    }
    catch (const vk::SystemError& e) {
      LOG(ERROR) << fmt::format("Failed to create debug messenger: {}", e.what());
      // Continue without debug messenger
    }
#endif

    // enumerate the physicalDevices
    vk::raii::PhysicalDevices physicalDevices(instance);

    if (physicalDevices.empty()) {
      LOG(WARNING) << "No Vulkan-compatible devices found";
    }

    // Go through the list of physical devices and select only those that are capable of running the API version we
    // want.
    std::vector<vk::raii::PhysicalDevice> desiredPhysicalDevices;
    for (const auto& physicalDevice : physicalDevices) {
      vk::PhysicalDeviceProperties deviceProperties = physicalDevice.getProperties();
      vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties();
      auto extensionProperties = physicalDevice.enumerateDeviceExtensionProperties();

      // Calculate dedicated GPU memory
      vk::DeviceSize dedicatedMemory = 0;
      for (uint32_t i = 0; i < memProperties.memoryHeapCount; i++) {
        if (memProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
          dedicatedMemory += memProperties.memoryHeaps[i].size;
        }
      }

      LOG(INFO) << fmt::format("Device Name:          {}", deviceProperties.deviceName);
      LOG(INFO) << fmt::format("API Version:          {} ({})",
                               versionToString(deviceProperties.apiVersion),
                               deviceProperties.apiVersion);
      LOG(INFO) << fmt::format("Driver Version:       {} ({})",
                               versionToString(deviceProperties.driverVersion),
                               deviceProperties.driverVersion);
      VLOG(1) << fmt::format("Vendor ID:            0x{:04x}", deviceProperties.vendorID);
      VLOG(1) << fmt::format("Device ID:            0x{:04x}", deviceProperties.deviceID);
      LOG(INFO) << fmt::format("Device Type:          {}", vk::to_string(deviceProperties.deviceType));
      VLOG(1) << fmt::format("Pipeline Cache UUID:  {}", uuidToString(deviceProperties.pipelineCacheUUID));
      LOG(INFO) << fmt::format("Dedicated GPU Memory: {} MB", dedicatedMemory / (1024 * 1024));
      VLOG(1) << "Supported Extensions: ";
      for (const auto& ext : extensionProperties) {
        VLOG(1) << fmt::format("  - {} (version {})", ext.extensionName, ext.specVersion);
      }
      LOG(INFO) << "-------------------------";

      if (desiredVersion <= physicalDevice.getProperties().apiVersion) {
        desiredPhysicalDevices.push_back(physicalDevice);
      }

      {
        // Create a logical device
        float queuePriority = 1.0f;
        vk::DeviceQueueCreateInfo queueCreateInfo{.queueFamilyIndex =
                                                    0, // Assume the first queue family supports graphics
                                                  .queueCount = 1,
                                                  .pQueuePriorities = &queuePriority};

        std::vector<const char*> deviceExtensions;
        // if (isExtensionAvailable(extensionProperties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        //   deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        // }

        vk::DeviceCreateInfo deviceCreateInfo{.queueCreateInfoCount = 1,
                                              .pQueueCreateInfos = &queueCreateInfo,
                                              .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
                                              .ppEnabledExtensionNames =
                                                deviceExtensions.empty() ? nullptr : deviceExtensions.data()};

        vk::raii::Device device(physicalDevices[0], deviceCreateInfo);

        // Test error callback with an invalid image creation
        try {
          // Attempt to create an invalid image to trigger a validation error
          vk::ImageCreateInfo invalidImageInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = vk::Extent3D{0, 1, 1}, // Invalid width of 0
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eColorAttachment
          };
          vk::raii::Image invalidImage(device, invalidImageInfo);
        }
        catch (const vk::SystemError& e) {
          LOG(INFO) << "Expected validation error triggered: " << e.what();
        }
      }
    }

    // If we have something in the desired version physical device list, we're good
    if (desiredPhysicalDevices.empty()) {
      LOG(ERROR) << "Current system can not use the desired Vulkan API version " << desiredVersionString;
    }
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan error: " << e.what();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Error: " << e.what();
  }
}

} // namespace nim