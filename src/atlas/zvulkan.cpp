#include "zvulkan.h"

#include "zvulkancontext.h"
#include "z3dglobalparameters.h"
#include "zvulkandevice.h"
#include "z3dscratchresourcepool.h"
#include "zglmutils.h"
#include "zexception.h"
#include "zlog.h"

namespace nim {

void initVulkan()
{
  try {
    LOG(INFO) << "Initializing Vulkan...";

    // Create Vulkan context which handles instance, device selection and initialization
    static std::unique_ptr<ZVulkanContext> vulkanContext = std::make_unique<ZVulkanContext>();

    // Create device for the engine
    auto device = vulkanContext->createDevice();

    // Example of querying device properties for verification
    auto deviceProperties = vulkanContext->physicalDevice().getProperties();
    LOG(INFO) << "Vulkan initialized successfully with device: " << deviceProperties.deviceName.data();
    auto version = deviceProperties.apiVersion;
    LOG(INFO) << fmt::format("API Version: {}.{}.{}",
                             VK_VERSION_MAJOR(version),
                             VK_VERSION_MINOR(version),
                             VK_VERSION_PATCH(version));

    // Vulkan compositor demo temporarily disabled while backend interfaces are reworked.

  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan system error: " << e.what();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Error initializing Vulkan: " << e.what();
  }
}

} // namespace nim
