#include "zvulkan.h"

#include "zlog.h"

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
    // instantiate a vk::raii::Context
    vk::raii::Context context;

    // Determine what API version is available
    uint32_t apiVersion = context.enumerateInstanceVersion();

    LOG(INFO) << fmt::format("Loader/Runtime support detected for Vulkan {}.{}.{}",
                             VK_VERSION_MAJOR(apiVersion),
                             VK_VERSION_MINOR(apiVersion),
                             VK_VERSION_PATCH(apiVersion));

    // Set the desired version we want
    constexpr uint16_t desiredMajorVersion = 1;
    constexpr uint16_t desiredMinorVersion = 2;
    constexpr uint32_t desiredVersion = VK_MAKE_VERSION(desiredMajorVersion, desiredMinorVersion, 0);
    std::string desiredVersionString = fmt::format("{}.{}.{}", desiredMajorVersion, desiredMinorVersion, 0);

    constexpr auto appInfo = vk::ApplicationInfo{.pApplicationName = "Vulkan Device Enumerator",
                                                 .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                                 .pEngineName = "No Engine",
                                                 .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                                 .apiVersion = desiredVersion};

    constexpr std::array enabledExtensions = {
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };

    const auto createInfo =
      vk::InstanceCreateInfo{.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
                             .pApplicationInfo = &appInfo,
                             .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                             .ppEnabledExtensionNames = enabledExtensions.data()};

    vk::raii::Instance instance(context, createInfo);

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

      if (desiredVersion <= physicalDevice.getProperties().apiVersion) {
        desiredPhysicalDevices.push_back(physicalDevice);
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