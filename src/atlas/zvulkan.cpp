#include "zvulkan.h"

#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanrendererbase.h"
#include "zvulkanbackgroundrenderer.h"
#include "zvulkancompositor.h"
#include "zexception.h"
#include "zlog.h"
#include <fmt/format.h>

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

    // Minimal compositor demo: background + axis lines + readback
    const uint32_t W = 320, H = 200;
    ZVulkanCompositor comp(*device, W, H);
    comp.setShowBackground(true);
    comp.setBackgroundMode(ZVulkanBackgroundRenderer::Mode::Gradient);
    comp.setBackgroundOrientation(ZVulkanBackgroundRenderer::GradientOrientation::TopToBottom);
    comp.setBackgroundColors(glm::vec4(0.05f, 0.07f, 0.09f, 1.0f), glm::vec4(0.25f, 0.27f, 0.29f, 1.0f));
    comp.setBackgroundRegion(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    comp.setShowAxis(true);

    auto pixels = comp.renderAndReadback();
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (auto b : pixels) { hash ^= b; hash *= 1099511628211ull; }
    LOG(INFO) << fmt::format("Vulkan compositor (bg+axis) readback hash: 0x{:016x}", hash);
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan system error: " << e.what();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Error initializing Vulkan: " << e.what();
  }
}

} // namespace nim
