#include "zvulkan.h"

#include "zlog.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

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

void test()
{
  try {
    // initialize minimal set of function pointers
    VULKAN_HPP_DEFAULT_DISPATCHER.init();

    // Log all available instance extensions
    std::vector<vk::ExtensionProperties> extensionProperties = vk::enumerateInstanceExtensionProperties();

    // LOG(INFO) << "Available Vulkan instance extensions:";
    // for (const auto& ext : extensionProperties) {
    //   LOG(INFO) << "  " << std::string_view(ext.extensionName.data(), std::strlen(ext.extensionName)) << " (version "
    //             << ext.specVersion << ")";
    // }
    // LOG(INFO) << "Total number of extensions: " << extensionProperties.size();

    // std::vector<const char*> enabledExtensions;
    // std::vector<const char*> enabledLayers;
    //
    // // Try to enable some extensions if they're available
    // auto addExtensionIfAvailable = [&](const char* extName) {
    //   if (std::any_of(extensionProperties.begin(),
    //                   extensionProperties.end(),
    //                   [extName](const vk::ExtensionProperties& prop) {
    //                     return strcmp(extName, prop.extensionName) == 0;
    //                   })) {
    //     enabledExtensions.push_back(extName);
    //     LOG(INFO) << "Enabling extension: " << extName;
    //   } else {
    //     LOG(WARNING) << "Extension not available: " << extName;
    //   }
    // };

    // addExtensionIfAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    // if (std::find(enabledExtensions.begin(), enabledExtensions.end(), VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    // !=
    //     enabledExtensions.end()) {
    //   createInfo.setFlags(vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR);
    // }

    constexpr auto appInfo = vk::ApplicationInfo{.pApplicationName = "Vulkan Device Enumerator",
                                                 .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                                 .pEngineName = "No Engine",
                                                 .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                                 .apiVersion = VK_API_VERSION_1_2};

    constexpr std::array enabledExtensions = {
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };

    const auto createInfo =
      vk::InstanceCreateInfo{.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
                             .pApplicationInfo = &appInfo,
                             .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                             .ppEnabledExtensionNames = enabledExtensions.data()};

    vk::UniqueInstance instance = vk::createInstanceUnique(createInfo);

    // initialize function pointers for instance
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance.get());

    std::vector<vk::PhysicalDevice> devices = instance->enumeratePhysicalDevices();

    if (devices.empty()) {
      LOG(WARNING) << "No Vulkan-compatible devices found";
    }

    for (const auto& device : devices) {
      vk::PhysicalDeviceProperties deviceProperties = device.getProperties();
      vk::PhysicalDeviceMemoryProperties memProperties = device.getMemoryProperties();

      // Calculate dedicated GPU memory
      vk::DeviceSize dedicatedMemory = 0;
      for (uint32_t i = 0; i < memProperties.memoryHeapCount; i++) {
        if (memProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
          dedicatedMemory += memProperties.memoryHeaps[i].size;
        }
      }

      LOG(INFO) << fmt::format("Device Name:          {}", std::string_view(deviceProperties.deviceName));
      LOG(INFO) << fmt::format("API Version:          {} ({})",
                               versionToString(deviceProperties.apiVersion),
                               deviceProperties.apiVersion);
      LOG(INFO) << fmt::format("Driver Version:       {} ({})",
                               versionToString(deviceProperties.driverVersion),
                               deviceProperties.driverVersion);
      LOG(INFO) << fmt::format("Vendor ID:            0x{:04x}", deviceProperties.vendorID);
      LOG(INFO) << fmt::format("Device ID:            0x{:04x}", deviceProperties.deviceID);
      LOG(INFO) << fmt::format("Device Type:          {}", vk::to_string(deviceProperties.deviceType));
      LOG(INFO) << fmt::format("Pipeline Cache UUID:  {}", uuidToString(deviceProperties.pipelineCacheUUID));
      LOG(INFO) << fmt::format("Dedicated GPU Memory: {} MB", dedicatedMemory / (1024 * 1024));
      LOG(INFO) << "-------------------------";
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