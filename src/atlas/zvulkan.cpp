#include "zvulkan.h"

#include <set>

DEFINE_bool(atlas_debug_vulkan,
            false,
            "Whether to enable debugging features of vulkan, default is false, can set to true for debugging");

namespace nim {

std::string uuidToString(const vk::ArrayWrapper1D<unsigned char, VK_UUID_SIZE>& uuid)
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
bool isLayerAvailable(const char* layerName, const std::vector<vk::LayerProperties>& availableLayers)
{
  return std::ranges::find_if(availableLayers, [layerName](const vk::LayerProperties& layerProperties) {
           return strcmp(layerName, layerProperties.layerName) == 0;
         }) != availableLayers.end();
}

bool addRequiredLayers(const char* layerName,
                       std::vector<const char*>& enabledExtensions,
                       const std::vector<vk::LayerProperties>& availableLayers,
                       bool isOptional = false)
{
  if (isLayerAvailable(layerName, availableLayers)) {
    enabledExtensions.push_back(layerName);
    return true;
  } else {
    if (!isOptional) {
      throw ZException(fmt::format("Required layer {} is not supported by the device.", layerName));
    } else {
      LOG(WARNING) << fmt::format("Optional layer {} is not supported by the device.", layerName);
    }
  }
  return false;
}

// Function to check if an extension is available
bool isExtensionAvailable(const char* extensionName, const std::vector<vk::ExtensionProperties>& availableExtensions)
{
  return std::ranges::find_if(availableExtensions, [extensionName](const vk::ExtensionProperties& extensionProperties) {
           return strcmp(extensionName, extensionProperties.extensionName) == 0;
         }) != availableExtensions.end();
}

bool addRequiredExtension(const char* extensionName,
                          std::vector<const char*>& enabledExtensions,
                          const std::vector<vk::ExtensionProperties>& availableExtensions,
                          bool isOptional = false)
{
  if (isExtensionAvailable(extensionName, availableExtensions)) {
    enabledExtensions.push_back(extensionName);
    return true;
  } else {
    if (!isOptional) {
      throw ZException(fmt::format("Required extension {} is not supported by the device.", extensionName));
    } else {
      LOG(WARNING) << fmt::format("Optional extension {} is not supported by the device.", extensionName);
    }
  }
  return false;
}

void logVulkan12Features(const vk::PhysicalDeviceVulkan12Features& features)
{
  if (!VLOG_IS_ON(1)) {
    return;
  }
  VLOG(1) << "Vulkan 1.2 Features:";
  VLOG(1) << fmt::format("  samplerMirrorClampToEdge: {}", features.samplerMirrorClampToEdge);
  VLOG(1) << fmt::format("  drawIndirectCount: {}", features.drawIndirectCount);
  VLOG(1) << fmt::format("  storageBuffer8BitAccess: {}", features.storageBuffer8BitAccess);
  VLOG(1) << fmt::format("  uniformAndStorageBuffer8BitAccess: {}", features.uniformAndStorageBuffer8BitAccess);
  VLOG(1) << fmt::format("  storagePushConstant8: {}", features.storagePushConstant8);
  VLOG(1) << fmt::format("  shaderBufferInt64Atomics: {}", features.shaderBufferInt64Atomics);
  VLOG(1) << fmt::format("  shaderSharedInt64Atomics: {}", features.shaderSharedInt64Atomics);
  VLOG(1) << fmt::format("  shaderFloat16: {}", features.shaderFloat16);
  VLOG(1) << fmt::format("  shaderInt8: {}", features.shaderInt8);
  VLOG(1) << fmt::format("  descriptorIndexing: {}", features.descriptorIndexing);
  VLOG(1) << fmt::format("  shaderInputAttachmentArrayDynamicIndexing: {}",
                         features.shaderInputAttachmentArrayDynamicIndexing);
  VLOG(1) << fmt::format("  shaderUniformTexelBufferArrayDynamicIndexing: {}",
                         features.shaderUniformTexelBufferArrayDynamicIndexing);
  VLOG(1) << fmt::format("  shaderStorageTexelBufferArrayDynamicIndexing: {}",
                         features.shaderStorageTexelBufferArrayDynamicIndexing);
  VLOG(1) << fmt::format("  shaderUniformBufferArrayNonUniformIndexing: {}",
                         features.shaderUniformBufferArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderSampledImageArrayNonUniformIndexing: {}",
                         features.shaderSampledImageArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderStorageBufferArrayNonUniformIndexing: {}",
                         features.shaderStorageBufferArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderStorageImageArrayNonUniformIndexing: {}",
                         features.shaderStorageImageArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderInputAttachmentArrayNonUniformIndexing: {}",
                         features.shaderInputAttachmentArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderUniformTexelBufferArrayNonUniformIndexing: {}",
                         features.shaderUniformTexelBufferArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  shaderStorageTexelBufferArrayNonUniformIndexing: {}",
                         features.shaderStorageTexelBufferArrayNonUniformIndexing);
  VLOG(1) << fmt::format("  descriptorBindingUniformBufferUpdateAfterBind: {}",
                         features.descriptorBindingUniformBufferUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingSampledImageUpdateAfterBind: {}",
                         features.descriptorBindingSampledImageUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingStorageImageUpdateAfterBind: {}",
                         features.descriptorBindingStorageImageUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingStorageBufferUpdateAfterBind: {}",
                         features.descriptorBindingStorageBufferUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingUniformTexelBufferUpdateAfterBind: {}",
                         features.descriptorBindingUniformTexelBufferUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingStorageTexelBufferUpdateAfterBind: {}",
                         features.descriptorBindingStorageTexelBufferUpdateAfterBind);
  VLOG(1) << fmt::format("  descriptorBindingUpdateUnusedWhilePending: {}",
                         features.descriptorBindingUpdateUnusedWhilePending);
  VLOG(1) << fmt::format("  descriptorBindingPartiallyBound: {}", features.descriptorBindingPartiallyBound);
  VLOG(1) << fmt::format("  descriptorBindingVariableDescriptorCount: {}",
                         features.descriptorBindingVariableDescriptorCount);
  VLOG(1) << fmt::format("  runtimeDescriptorArray: {}", features.runtimeDescriptorArray);
  VLOG(1) << fmt::format("  samplerFilterMinmax: {}", features.samplerFilterMinmax);
  VLOG(1) << fmt::format("  scalarBlockLayout: {}", features.scalarBlockLayout);
  VLOG(1) << fmt::format("  imagelessFramebuffer: {}", features.imagelessFramebuffer);
  VLOG(1) << fmt::format("  uniformBufferStandardLayout: {}", features.uniformBufferStandardLayout);
  VLOG(1) << fmt::format("  shaderSubgroupExtendedTypes: {}", features.shaderSubgroupExtendedTypes);
  VLOG(1) << fmt::format("  separateDepthStencilLayouts: {}", features.separateDepthStencilLayouts);
  VLOG(1) << fmt::format("  hostQueryReset: {}", features.hostQueryReset);
  VLOG(1) << fmt::format("  timelineSemaphore: {}", features.timelineSemaphore);
  VLOG(1) << fmt::format("  bufferDeviceAddress: {}", features.bufferDeviceAddress);
  VLOG(1) << fmt::format("  bufferDeviceAddressCaptureReplay: {}", features.bufferDeviceAddressCaptureReplay);
  VLOG(1) << fmt::format("  bufferDeviceAddressMultiDevice: {}", features.bufferDeviceAddressMultiDevice);
  VLOG(1) << fmt::format("  vulkanMemoryModel: {}", features.vulkanMemoryModel);
  VLOG(1) << fmt::format("  vulkanMemoryModelDeviceScope: {}", features.vulkanMemoryModelDeviceScope);
  VLOG(1) << fmt::format("  vulkanMemoryModelAvailabilityVisibilityChains: {}",
                         features.vulkanMemoryModelAvailabilityVisibilityChains);
  VLOG(1) << fmt::format("  shaderOutputViewportIndex: {}", features.shaderOutputViewportIndex);
  VLOG(1) << fmt::format("  shaderOutputLayer: {}", features.shaderOutputLayer);
  VLOG(1) << fmt::format("  subgroupBroadcastDynamicId: {}", features.subgroupBroadcastDynamicId);
}

void logVulkan13Features(const vk::PhysicalDeviceVulkan13Features& features)
{
  if (!VLOG_IS_ON(1)) {
    return;
  }
  VLOG(1) << "Vulkan 1.3 Features:";
  VLOG(1) << fmt::format("  robustImageAccess: {}", features.robustImageAccess);
  VLOG(1) << fmt::format("  inlineUniformBlock: {}", features.inlineUniformBlock);
  VLOG(1) << fmt::format("  descriptorBindingInlineUniformBlockUpdateAfterBind: {}",
                         features.descriptorBindingInlineUniformBlockUpdateAfterBind);
  VLOG(1) << fmt::format("  pipelineCreationCacheControl: {}", features.pipelineCreationCacheControl);
  VLOG(1) << fmt::format("  privateData: {}", features.privateData);
  VLOG(1) << fmt::format("  shaderDemoteToHelperInvocation: {}", features.shaderDemoteToHelperInvocation);
  VLOG(1) << fmt::format("  shaderTerminateInvocation: {}", features.shaderTerminateInvocation);
  VLOG(1) << fmt::format("  subgroupSizeControl: {}", features.subgroupSizeControl);
  VLOG(1) << fmt::format("  computeFullSubgroups: {}", features.computeFullSubgroups);
  VLOG(1) << fmt::format("  synchronization2: {}", features.synchronization2);
  VLOG(1) << fmt::format("  textureCompressionASTC_HDR: {}", features.textureCompressionASTC_HDR);
  VLOG(1) << fmt::format("  shaderZeroInitializeWorkgroupMemory: {}", features.shaderZeroInitializeWorkgroupMemory);
  VLOG(1) << fmt::format("  dynamicRendering: {}", features.dynamicRendering);
  VLOG(1) << fmt::format("  shaderIntegerDotProduct: {}", features.shaderIntegerDotProduct);
  VLOG(1) << fmt::format("  maintenance4: {}", features.maintenance4);
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsMessengerCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                           vk::DebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
                                                           const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                           void* /*pUserData*/)
{
  // Determine the appropriate log level based on message severity
  if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) {
    // For verbose messages, use VLOG(1)
    VLOG(1) << pCallbackData->pMessage;
    return VK_FALSE; // Return early since we already logged the verbose message
  }
  auto logLevel = google::GLOG_INFO;
  if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
    logLevel = google::GLOG_WARNING;
  } else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
    logLevel = google::GLOG_ERROR;
  }

  std::string message = pCallbackData->pMessage;

  if (pCallbackData->queueLabelCount > 0) {
    message += "\n\tQueue Labels:";
    for (uint32_t i = 0; i < pCallbackData->queueLabelCount; i++) {
      fmt::format_to(std::back_inserter(message), "\n\t\tlabelName = <{}>", pCallbackData->pQueueLabels[i].pLabelName);
    }
  }

  if (pCallbackData->cmdBufLabelCount > 0) {
    message += "\n\tCommandBuffer Labels:";
    for (uint32_t i = 0; i < pCallbackData->cmdBufLabelCount; i++) {
      fmt::format_to(std::back_inserter(message), "\n\t\tlabelName = <{}>", pCallbackData->pCmdBufLabels[i].pLabelName);
    }
  }

  google::LogMessage(__FILE__, __LINE__, logLevel).stream() << message;

  return VK_FALSE;
}

// Create Vulkan instance
vk::raii::Instance createVulkanInstance(vk::raii::Context& context)
{
  vk::ApplicationInfo appInfo{.pApplicationName = "Atlas",
                              .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                              .pEngineName = "Atlas",
                              .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                              .apiVersion = VK_MAKE_VERSION(1, 3, 0)};

  // Get available layers and extensions
  std::vector<vk::LayerProperties> availableLayers = context.enumerateInstanceLayerProperties();
  LOG(INFO) << fmt::format("Available Instance Layers ({})", availableLayers.size());
  for (const auto& layer : availableLayers) {
    LOG(INFO) << fmt::format("  Layer Name: {} (Description: {}, Spec Version: {}.{}.{}, Implementation Version: {})",
                             layer.layerName.data(),
                             layer.description.data(),
                             VK_VERSION_MAJOR(layer.specVersion),
                             VK_VERSION_MINOR(layer.specVersion),
                             VK_VERSION_PATCH(layer.specVersion),
                             layer.implementationVersion);
  }

  std::vector<vk::ExtensionProperties> availableExtensions = context.enumerateInstanceExtensionProperties();
  LOG(INFO) << fmt::format("Available Instance Extensions ({})", availableExtensions.size());
  for (const auto& extension : availableExtensions) {
    LOG(INFO) << fmt::format("  Extension Name: {} (Spec Version: {})",
                             extension.extensionName.data(),
                             extension.specVersion);
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

  std::vector<const char*> enabledLayers;
  // enabledLayers.push_back("VK_LAYER_NONEXISTENT_FOR_TESTING");
  std::vector<const char*> enabledExtensions;
  vk::InstanceCreateFlags instanceFlags;

  if (FLAGS_atlas_debug_vulkan) {
    // Enable validation layers in debug mode
    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
    addRequiredLayers(validationLayerName, enabledLayers, availableLayers, true);

    // Enable debug utils extension
    addRequiredExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, enabledExtensions, availableExtensions, true);
  }

#ifdef __APPLE__
  // Enable portability enumeration on macOS
  addRequiredExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, enabledExtensions, availableExtensions, true);
  instanceFlags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif

  // Create the Vulkan instance
  vk::InstanceCreateInfo instanceCreateInfo{.flags = instanceFlags,
                                            .pApplicationInfo = &appInfo,
                                            .enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
                                            .ppEnabledLayerNames = enabledLayers.data(),
                                            .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                                            .ppEnabledExtensionNames = enabledExtensions.data()};

  vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfo{
    .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                       vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                       vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
    .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                   vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
    .pfnUserCallback = debugUtilsMessengerCallback};

  if (FLAGS_atlas_debug_vulkan) {
    instanceCreateInfo.setPNext(&debugUtilsMessengerCreateInfo);
  }

  vk::raii::Instance instance(context, instanceCreateInfo);
  LOG(INFO) << "Vulkan instance created successfully";

  // Log enabled layers and extensions
  LOG(INFO) << "Enabled Layers:";
  for (const auto& layer : enabledLayers) {
    LOG(INFO) << "  " << layer;
  }
  LOG(INFO) << "Enabled Extensions:";
  for (const auto& extension : enabledExtensions) {
    LOG(INFO) << "  " << extension;
  }

  return instance;
}

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
    LOG(INFO) << "API Version: " << versionToString(deviceProperties.apiVersion);
    
  } catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan system error: " << e.what();
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error initializing Vulkan: " << e.what();
  }
}

// ZVulkanContext implementation
ZVulkanContext::ZVulkanContext()
{
  try {
    // Initialize Vulkan context
    m_context.emplace();
    createInstance();
    setupDebugMessenger();
    pickPhysicalDevice();
    createLogicalDevice();
    createCommandPool();
    LOG(INFO) << "ZVulkanContext initialized successfully";
  } catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan system error: " << e.what();
    throw ZException(fmt::format("Vulkan system error: {}", e.what()));
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error initializing ZVulkanContext: " << e.what();
    throw ZException(fmt::format("Error initializing ZVulkanContext: {}", e.what()));
  }
}

ZVulkanContext::~ZVulkanContext()
{
  LOG(INFO) << "Destroying ZVulkanContext";
  // RAII will handle cleanup in reverse order
}

void ZVulkanContext::createInstance()
{
  vk::ApplicationInfo appInfo{
    .pApplicationName = "Atlas",
    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
    .pEngineName = "Z3D Vulkan Engine",
    .engineVersion = VK_MAKE_VERSION(1, 0, 0),
    .apiVersion = VK_MAKE_VERSION(1, 3, 0)
  };

  // Get available layers and extensions
  std::vector<vk::LayerProperties> availableLayers = m_context->enumerateInstanceLayerProperties();
  std::vector<vk::ExtensionProperties> availableExtensions = m_context->enumerateInstanceExtensionProperties();

  std::vector<const char*> enabledLayers;
  std::vector<const char*> enabledExtensions;
  vk::InstanceCreateFlags instanceFlags;

    if (FLAGS_atlas_debug_vulkan) {
    // Enable validation layers in debug mode
    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
    addRequiredLayers(validationLayerName, enabledLayers, availableLayers, true);

    // Enable debug utils extension
    addRequiredExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, enabledExtensions, availableExtensions, true);
  }

#ifdef __APPLE__
  // Enable portability enumeration on macOS
  addRequiredExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, enabledExtensions, availableExtensions, true);
  instanceFlags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif

  // Create the Vulkan instance
  vk::InstanceCreateInfo instanceCreateInfo{
    .flags = instanceFlags,
    .pApplicationInfo = &appInfo,
    .enabledLayerCount = static_cast<uint32_t>(enabledLayers.size()),
    .ppEnabledLayerNames = enabledLayers.data(),
    .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
    .ppEnabledExtensionNames = enabledExtensions.data()
  };

  vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfo{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
    .pfnUserCallback = debugUtilsMessengerCallback
  };

  if (FLAGS_atlas_debug_vulkan) {
    instanceCreateInfo.setPNext(&debugUtilsMessengerCreateInfo);
  }

  m_instance.emplace(*m_context, instanceCreateInfo);
  LOG(INFO) << "Vulkan instance created successfully";
}

void ZVulkanContext::setupDebugMessenger()
{
  if (!FLAGS_atlas_debug_vulkan || !m_instance) {
    return;
  }

  vk::DebugUtilsMessengerCreateInfoEXT createInfo{
    .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                       vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                       vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
    .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                   vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                   vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
    .pfnUserCallback = debugUtilsMessengerCallback
  };

  try {
    m_debugMessenger.emplace(*m_instance, createInfo);
        LOG(INFO) << "Debug messenger created successfully";
  } catch (const vk::SystemError& e) {
        LOG(ERROR) << fmt::format("Failed to create debug messenger: {}", e.what());
        // Continue without debug messenger
      }
    }

void ZVulkanContext::pickPhysicalDevice()
{
  vk::raii::PhysicalDevices physicalDevices(*m_instance);

    if (physicalDevices.empty()) {
    throw ZException("No Vulkan-compatible devices found");
  }

  // Go through the list of physical devices and select the first suitable one
  for (auto& physicalDevice : physicalDevices) {
      auto deviceProperties = physicalDevice.getProperties();
      auto deviceExtensionProperties = physicalDevice.enumerateDeviceExtensionProperties();

    LOG(INFO) << fmt::format("Evaluating device: {}", deviceProperties.deviceName.data());
    
    // Check if device supports required features
    if (deviceProperties.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      LOG(INFO) << "  Device doesn't support Vulkan 1.2, skipping";
      continue;
    }
    
    // Check if device has required extensions
    if (!checkDeviceExtensionSupport(physicalDevice)) {
      LOG(INFO) << "  Device doesn't support required extensions, skipping";
      continue;
    }
    
    // Check if device has required queue families
    QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
    if (!indices.isComplete()) {
      LOG(INFO) << "  Device doesn't have required queue families, skipping";
      continue;
    }
    
    // This device is suitable
    m_physicalDevice.emplace(std::move(physicalDevice));
    m_queueFamilyIndices = indices;
    
    auto selectedProperties = m_physicalDevice->getProperties();
    LOG(INFO) << fmt::format("Selected physical device: {}", selectedProperties.deviceName.data());
    LOG(INFO) << fmt::format("  API Version: {}", versionToString(selectedProperties.apiVersion));
    LOG(INFO) << fmt::format("  Driver Version: {}", versionToString(selectedProperties.driverVersion));
    
    // Enable all available Vulkan 1.2 and 1.3 features
    vk::PhysicalDeviceFeatures2 features2;
    vk::PhysicalDeviceVulkan11Features vulkan11Features;
    vk::PhysicalDeviceVulkan12Features vulkan12Features;
    vk::PhysicalDeviceVulkan13Features vulkan13Features;
    
    features2.pNext = &vulkan11Features;
    vulkan11Features.pNext = &vulkan12Features;
    vulkan12Features.pNext = &vulkan13Features;
    
    auto features = m_physicalDevice->getFeatures2();
    
    // Log Vulkan 1.2 features
      if (VLOG_IS_ON(1)) {
        logVulkan12Features(vulkan12Features);
        logVulkan13Features(vulkan13Features);
    }
    
    return;
  }
  
  throw ZException("Failed to find a suitable GPU");
}

ZVulkanContext::QueueFamilyIndices ZVulkanContext::findQueueFamilies(vk::raii::PhysicalDevice& physicalDevice) const
{
  QueueFamilyIndices indices;
  
  std::vector<vk::QueueFamilyProperties> queueFamilies = physicalDevice.getQueueFamilyProperties();
  
  // Find queue family with graphics support
  for (uint32_t i = 0; i < queueFamilies.size(); i++) {
    if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics) {
      indices.graphicsFamily = i;
    }
    
    // For now, using the same queue for presentation
    indices.presentFamily = i;
    
    if (indices.isComplete()) {
      break;
    }
  }
  
  return indices;
}

bool ZVulkanContext::checkDeviceExtensionSupport(vk::raii::PhysicalDevice& physicalDevice) const
{
  std::vector<vk::ExtensionProperties> availableExtensions = physicalDevice.enumerateDeviceExtensionProperties();
  
  std::vector<const char*> requiredExtensions;
  
  // Add required extensions based on platform
#ifdef __APPLE__
  requiredExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
  
  // Add required extensions for functionality
  auto deviceProperties = physicalDevice.getProperties();
  if (deviceProperties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    // For Vulkan 1.2, we need these extensions to get 1.3-like functionality
    requiredExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    requiredExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
  }
  
  // Check if all required extensions are supported
  for (const auto& requiredExt : requiredExtensions) {
    bool found = false;
    for (const auto& availableExt : availableExtensions) {
      if (strcmp(requiredExt, availableExt.extensionName) == 0) {
        found = true;
        break;
      }
    }
    
    if (!found) {
      LOG(INFO) << "  Missing required extension: " << requiredExt;
      return false;
    }
  }
  
  return true;
}

void ZVulkanContext::createLogicalDevice()
{
  if (!m_physicalDevice) {
    throw ZException("Attempted to create logical device without a physical device");
  }
  
  // Create device with a single queue
  float queuePriority = 1.0f;
  
  std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueQueueFamilies = {
    m_queueFamilyIndices.graphicsFamily.value(),
    m_queueFamilyIndices.presentFamily.value()
  };
  
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    vk::DeviceQueueCreateInfo queueCreateInfo{
      .queueFamilyIndex = queueFamily,
      .queueCount = 1,
      .pQueuePriorities = &queuePriority
    };
    queueCreateInfos.push_back(queueCreateInfo);
  }
  
  // Specify required device features
  auto features2 = m_physicalDevice->getFeatures2<vk::PhysicalDeviceFeatures2,
                                                vk::PhysicalDeviceVulkan12Features,
                                                vk::PhysicalDeviceVulkan13Features>();
  auto& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
  auto& physicalDeviceVulkan12Features = features2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& physicalDeviceVulkan13Features = features2.get<vk::PhysicalDeviceVulkan13Features>();
  
  // Setup enabled features
      vk::StructureChain<vk::PhysicalDeviceFeatures2,
                         vk::PhysicalDeviceVulkan12Features,
                         vk::PhysicalDeviceVulkan13Features>
        enabledFeatures2;
      auto& enabledPhysicalDeviceFeatures2 = enabledFeatures2.get<vk::PhysicalDeviceFeatures2>();
      auto& enabledPhysicalDeviceVulkan12Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan12Features>();
      auto& enabledPhysicalDeviceVulkan13Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan13Features>();
  
  // Enable basic features
  enabledPhysicalDeviceFeatures2.features.samplerAnisotropy = physicalDeviceFeatures.samplerAnisotropy;
  enabledPhysicalDeviceFeatures2.features.fillModeNonSolid = physicalDeviceFeatures.fillModeNonSolid;
  
  // Get device properties to determine API version
  auto deviceProperties = m_physicalDevice->getProperties();
  
  // Enable required extensions
  std::vector<const char*> enabledExtensions;

      if (deviceProperties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    // Enable Vulkan 1.3 features
        enabledPhysicalDeviceVulkan13Features.dynamicRendering = true;
        enabledPhysicalDeviceVulkan13Features.synchronization2 = true;
  } else {
    // For Vulkan 1.2, use extensions
        enabledPhysicalDeviceVulkan12Features.descriptorIndexing = true;
        enabledPhysicalDeviceVulkan12Features.bufferDeviceAddress = true;
        enabledFeatures2.unlink<vk::PhysicalDeviceVulkan13Features>();
    
    auto deviceExtensionProperties = m_physicalDevice->enumerateDeviceExtensionProperties();
        addRequiredExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                         enabledExtensions,
                             deviceExtensionProperties,
                             false);
        addRequiredExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                         enabledExtensions,
                             deviceExtensionProperties,
                             false);
      }

  // Add platform-specific required extensions
#ifdef __APPLE__
  auto deviceExtensionProperties = m_physicalDevice->enumerateDeviceExtensionProperties();
      addRequiredExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
                       enabledExtensions,
                           deviceExtensionProperties,
                           true);
#endif

  // Create the logical device
  vk::DeviceCreateInfo deviceCreateInfo{
    .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
    .pQueueCreateInfos = queueCreateInfos.data(),
    .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
    .ppEnabledExtensionNames = enabledExtensions.data()
  };
  
  deviceCreateInfo.pNext = &enabledPhysicalDeviceFeatures2;
  
  m_device.emplace(*m_physicalDevice, deviceCreateInfo);
  LOG(INFO) << "Logical device created successfully";
  
  // Get device queues
  m_graphicsQueue.emplace(*m_device, m_queueFamilyIndices.graphicsFamily.value(), 0);
  m_presentQueue.emplace(*m_device, m_queueFamilyIndices.presentFamily.value(), 0);
  LOG(INFO) << "Device queues retrieved successfully";
}

void ZVulkanContext::createCommandPool()
{
  vk::CommandPoolCreateInfo poolInfo{
    .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
    .queueFamilyIndex = m_queueFamilyIndices.graphicsFamily.value()
  };
  
  m_commandPool.emplace(*m_device, poolInfo);
  LOG(INFO) << "Command pool created successfully";
}

std::unique_ptr<ZVulkanDevice> ZVulkanContext::createDevice()
{
  return std::make_unique<ZVulkanDevice>(*this);
}

// ZVulkanDevice implementation
ZVulkanDevice::ZVulkanDevice(ZVulkanContext& context)
  : m_context(context)
{
  LOG(INFO) << "ZVulkanDevice created";
}

ZVulkanDevice::~ZVulkanDevice()
{
  LOG(INFO) << "Destroying ZVulkanDevice";
}

std::unique_ptr<ZVulkanBuffer> ZVulkanDevice::createBuffer(
    size_t size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags properties)
{
  return std::make_unique<ZVulkanBuffer>(*this, size, usage, properties);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(
    uint32_t width,
    uint32_t height,
    vk::Format format)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(
    uint32_t width,
    uint32_t height,
    vk::Format format,
    vk::ImageUsageFlags usage,
    vk::MemoryPropertyFlags memoryProperties)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format, usage, memoryProperties);
}

std::unique_ptr<ZVulkanShader> ZVulkanDevice::createShader(
    const std::string& vertexCode,
    const std::string& fragmentCode)
{
  return std::make_unique<ZVulkanShader>(*this, vertexCode, fragmentCode);
}

std::unique_ptr<ZVulkanPipeline> ZVulkanDevice::createPipeline(
    ZVulkanShader& shader,
    const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
    const vk::PrimitiveTopology& topology)
{
  return std::make_unique<ZVulkanPipeline>(*this, shader, vertexInputInfo, topology);
}

vk::raii::CommandBuffer ZVulkanDevice::beginSingleTimeCommands()
{
  // Create command buffer allocation info with C++20 style initialization
  const vk::CommandBufferAllocateInfo allocInfo{
    .commandPool = m_context.commandPool(),
    .level = vk::CommandBufferLevel::ePrimary,
    .commandBufferCount = 1
  };
  
  // Allocate command buffers using RAII wrapper
  vk::raii::CommandBuffers commandBuffers(m_context.device(), allocInfo);
  
  // Create command buffer begin info with C++20 style initialization
  const vk::CommandBufferBeginInfo beginInfo{
    .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
  };
  
  // Begin recording commands
  commandBuffers[0].begin(beginInfo);
  
  // Return the command buffer with move semantics to transfer ownership
  return std::move(commandBuffers[0]);
}

void ZVulkanDevice::endSingleTimeCommands(vk::raii::CommandBuffer& commandBuffer)
{
  // End command buffer recording
  commandBuffer.end();
  
  // Create submit info with C++20 style initialization
  vk::SubmitInfo submitInfo{
    .commandBufferCount = 1,
    .pCommandBuffers = &(*commandBuffer)
  };
  
  // Submit command buffer to graphics queue and wait for completion
  // Use RAII patterns for queue submission and synchronization
  m_context.graphicsQueue().submit(submitInfo, nullptr);
  m_context.graphicsQueue().waitIdle();
}

// Add method to create descriptor pool and descriptor set
std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createDescriptorPool()
{
  return std::make_unique<ZVulkanDescriptorPool>(*this);
}

std::unique_ptr<ZVulkanDescriptorSet> ZVulkanDevice::createDescriptorSet(
    ZVulkanDescriptorPool& pool, 
    vk::DescriptorSetLayout layout)
{
  auto descriptorSet = pool.allocateDescriptorSet(layout);
  return std::make_unique<ZVulkanDescriptorSet>(*this, std::move(descriptorSet));
}

std::unique_ptr<ZVulkanSwapChain> ZVulkanDevice::createSwapChain(
    uint32_t width, 
    uint32_t height)
{
  return std::make_unique<ZVulkanSwapChain>(*this, width, height);
}

// ZVulkanShader implementation
ZVulkanShader::ZVulkanShader(ZVulkanDevice& device, 
                            const std::string& vertexCode, 
                            const std::string& fragmentCode)
  : m_device(device)
{
  // Create vertex shader module
  createShaderModule(vertexCode, vk::ShaderStageFlagBits::eVertex);
  
  // Create fragment shader module
  createShaderModule(fragmentCode, vk::ShaderStageFlagBits::eFragment);
  
  LOG(INFO) << "ZVulkanShader created with vertex and fragment shaders";
}

ZVulkanShader::~ZVulkanShader()
{
  LOG(INFO) << "Destroying ZVulkanShader";
}

void ZVulkanShader::createShaderModule(const std::string& code, vk::ShaderStageFlagBits stage)
{
  // For simplicity, we're using GLSL source directly here
  // In a real-world application, you'd compile GLSL to SPIR-V
  LOG(INFO) << "Creating shader module with GLSL source code (should be compiled to SPIR-V in production)";
  
  // In a real implementation, you would load a SPIR-V binary and create a shader module:
  // vk::ShaderModuleCreateInfo createInfo{
  //   .codeSize = spirvCode.size(),
  //   .pCode = reinterpret_cast<const uint32_t*>(spirvCode.data())
  // };
  
  // For demonstration purposes, we're just storing the source code
  if (stage == vk::ShaderStageFlagBits::eVertex) {
    m_vertexCode = code;
  } else if (stage == vk::ShaderStageFlagBits::eFragment) {
    m_fragmentCode = code;
  }
  
  // Store shader stage info for pipeline creation
  vk::PipelineShaderStageCreateInfo shaderStageInfo{
    .stage = stage,
    // In a real implementation, this would point to an actual shader module:
    // .module = *shaderModule,
    .pName = "main",
    .pSpecializationInfo = nullptr
  };
  
  m_shaderStages.push_back(shaderStageInfo);
}

const std::vector<vk::PipelineShaderStageCreateInfo>& ZVulkanShader::shaderStages() const
{
  return m_shaderStages;
}

// ZVulkanPipeline implementation
ZVulkanPipeline::ZVulkanPipeline(ZVulkanDevice& device, 
                                ZVulkanShader& shader, 
                                const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                                const vk::PrimitiveTopology& topology)
  : m_device(device)
  , m_shader(shader)
  , m_vertexInputInfo(vertexInputInfo)
  , m_topology(topology)
{
  LOG(INFO) << "ZVulkanPipeline created";
}

ZVulkanPipeline::~ZVulkanPipeline()
{
  LOG(INFO) << "Destroying ZVulkanPipeline";
}

void ZVulkanPipeline::setDescriptorSetLayouts(const std::vector<vk::DescriptorSetLayout>& layouts)
{
  m_descriptorSetLayouts = layouts;
}

void ZVulkanPipeline::setPushConstantRanges(const std::vector<vk::PushConstantRange>& pushConstantRanges)
{
  m_pushConstantRanges = pushConstantRanges;
}

void ZVulkanPipeline::create()
{
  // Create pipeline layout
  vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
    .setLayoutCount = static_cast<uint32_t>(m_descriptorSetLayouts.size()),
    .pSetLayouts = m_descriptorSetLayouts.empty() ? nullptr : m_descriptorSetLayouts.data(),
    .pushConstantRangeCount = static_cast<uint32_t>(m_pushConstantRanges.size()),
    .pPushConstantRanges = m_pushConstantRanges.empty() ? nullptr : m_pushConstantRanges.data()
  };
  
  m_pipelineLayout.emplace(m_device.context().device(), pipelineLayoutInfo);
  
  // Define the pipeline
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
    .topology = m_topology,
    .primitiveRestartEnable = VK_FALSE
  };
  
  // Default viewport and scissor state - will be set dynamically
  vk::PipelineViewportStateCreateInfo viewportState{
    .viewportCount = 1,
    .pViewports = nullptr, // Dynamic state
    .scissorCount = 1,
    .pScissors = nullptr // Dynamic state
  };
  
  vk::PipelineRasterizationStateCreateInfo rasterizer{
    .depthClampEnable = VK_FALSE,
    .rasterizerDiscardEnable = VK_FALSE,
    .polygonMode = vk::PolygonMode::eFill,
    .cullMode = vk::CullModeFlagBits::eBack,
    .frontFace = vk::FrontFace::eCounterClockwise,
    .depthBiasEnable = VK_FALSE,
    .depthBiasConstantFactor = 0.0f,
    .depthBiasClamp = 0.0f,
    .depthBiasSlopeFactor = 0.0f,
    .lineWidth = 1.0f
  };
  
  vk::PipelineMultisampleStateCreateInfo multisampling{
    .rasterizationSamples = vk::SampleCountFlagBits::e1,
    .sampleShadingEnable = VK_FALSE,
    .minSampleShading = 1.0f,
    .pSampleMask = nullptr,
    .alphaToCoverageEnable = VK_FALSE,
    .alphaToOneEnable = VK_FALSE
  };
  
  vk::PipelineDepthStencilStateCreateInfo depthStencil{
    .depthTestEnable = VK_TRUE,
    .depthWriteEnable = VK_TRUE,
    .depthCompareOp = vk::CompareOp::eLess,
    .depthBoundsTestEnable = VK_FALSE,
    .stencilTestEnable = VK_FALSE,
    .front = {},
    .back = {},
    .minDepthBounds = 0.0f,
    .maxDepthBounds = 1.0f
  };
  
  vk::PipelineColorBlendAttachmentState colorBlendAttachment{
    .blendEnable = VK_TRUE,
    .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
    .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
    .colorBlendOp = vk::BlendOp::eAdd,
    .srcAlphaBlendFactor = vk::BlendFactor::eOne,
    .dstAlphaBlendFactor = vk::BlendFactor::eZero,
    .alphaBlendOp = vk::BlendOp::eAdd,
    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | 
                      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
  };
  
  vk::PipelineColorBlendStateCreateInfo colorBlending{
    .logicOpEnable = VK_FALSE,
    .logicOp = vk::LogicOp::eCopy,
    .attachmentCount = 1,
    .pAttachments = &colorBlendAttachment,
    .blendConstants = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
  };
  
  // Dynamic states for viewport and scissor
  std::array<vk::DynamicState, 2> dynamicStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor
  };
  
  vk::PipelineDynamicStateCreateInfo dynamicState{
    .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
    .pDynamicStates = dynamicStates.data()
  };
  
  // Get shader stages from shader
  const auto& shaderStages = m_shader.shaderStages();
  
  // For this example, we're using dynamic rendering instead of render pass
  vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm;
  vk::Format depthFormat = vk::Format::eD32Sfloat;
  
  vk::PipelineRenderingCreateInfo renderingCreateInfo{};
  renderingCreateInfo.colorAttachmentCount = 1;
  renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
  renderingCreateInfo.depthAttachmentFormat = depthFormat;
  
  vk::GraphicsPipelineCreateInfo pipelineInfo{
    .pNext = &renderingCreateInfo,
    .stageCount = static_cast<uint32_t>(shaderStages.size()),
    .pStages = shaderStages.data(),
    .pVertexInputState = &m_vertexInputInfo,
    .pInputAssemblyState = &inputAssembly,
    .pTessellationState = nullptr,
    .pViewportState = &viewportState,
    .pRasterizationState = &rasterizer,
    .pMultisampleState = &multisampling,
    .pDepthStencilState = &depthStencil,
    .pColorBlendState = &colorBlending,
    .pDynamicState = &dynamicState,
    .layout = *m_pipelineLayout,
    .renderPass = nullptr, // Using dynamic rendering
    .subpass = 0,
    .basePipelineHandle = nullptr,
    .basePipelineIndex = -1
  };
  
  try {
    m_pipeline.emplace(m_device.context().device(), nullptr, pipelineInfo);
    LOG(INFO) << "Successfully created graphics pipeline";
  } catch (const vk::SystemError& e) {
    LOG(ERROR) << "Failed to create graphics pipeline: " << e.what();
    throw ZException(fmt::format("Failed to create graphics pipeline: {}", e.what()));
  }
}

// ZVulkanTexture implementation
ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device, uint32_t width, uint32_t height, vk::Format format)
  : m_device(device)
  , m_width(width)
  , m_height(height)
  , m_format(format)
  , m_usage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst)
  , m_memoryProperties(vk::MemoryPropertyFlagBits::eDeviceLocal)
  , m_currentLayout(vk::ImageLayout::eUndefined)
{
  createImage();
  createImageView();
  LOG(INFO) << "ZVulkanTexture created: " << width << "x" << height << ", format: " << vk::to_string(format);
}

ZVulkanTexture::ZVulkanTexture(ZVulkanDevice& device, 
                              uint32_t width, 
                              uint32_t height, 
                              vk::Format format,
                              vk::ImageUsageFlags usage,
                              vk::MemoryPropertyFlags memoryProperties)
  : m_device(device)
  , m_width(width)
  , m_height(height)
  , m_format(format)
  , m_usage(usage)
  , m_memoryProperties(memoryProperties)
{
  createImage();
  createImageView();
  LOG(INFO) << "ZVulkanTexture created with custom usage: " << width << "x" << height;
}

ZVulkanTexture::~ZVulkanTexture()
{
  LOG(INFO) << "Destroying ZVulkanTexture";
}

void ZVulkanTexture::createImage()
{
  // Create the image info with C++20 designated initializers and consistent indentation
  vk::ImageCreateInfo imageInfo{
    .imageType = vk::ImageType::e2D,
    .format = m_format,
    .extent = {
      .width = m_width,
      .height = m_height, 
      .depth = 1
    },
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = vk::SampleCountFlagBits::e1,
    .tiling = vk::ImageTiling::eOptimal,
    .usage = m_usage,
    .sharingMode = vk::SharingMode::eExclusive,
    .initialLayout = vk::ImageLayout::eUndefined
  };
  
  try {
    // Create image RAII object
    m_image.emplace(m_device.context().device(), imageInfo);
    
    // Get memory requirements using RAII object
    const vk::MemoryRequirements memRequirements = m_image->getMemoryRequirements();
    
    // Get memory properties using RAII object
    const vk::PhysicalDeviceMemoryProperties memProperties = 
        m_device.context().physicalDevice().getMemoryProperties();
    
    // Find a suitable memory type for device local memory
    uint32_t memoryTypeIndex = 0;
    bool foundSuitableMemory = false;
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((memRequirements.memoryTypeBits & (1 << i)) && 
          (memProperties.memoryTypes[i].propertyFlags & m_memoryProperties) == m_memoryProperties) {
        memoryTypeIndex = i;
        foundSuitableMemory = true;
        break;
      }
    }
    
    if (!foundSuitableMemory) {
      throw ZException("Failed to find suitable memory type for texture");
    }
    
    vk::MemoryAllocateInfo allocInfo{
      .allocationSize = memRequirements.size,
      .memoryTypeIndex = memoryTypeIndex
    };
    
    m_imageMemory.emplace(m_device.context().device(), allocInfo);
    m_image->bindMemory(*m_imageMemory, 0);
    
  } catch (const vk::SystemError& e) {
    LOG(ERROR) << "Failed to create image: " << e.what();
    throw ZException(fmt::format("Failed to create image: {}", e.what()));
  }
}

void ZVulkanTexture::createImageView()
{
  // Create image view info with C++20 designated initializers
  vk::ImageViewCreateInfo viewInfo{
    .image = *m_image,
    .viewType = vk::ImageViewType::e2D,
    .format = m_format,
    .subresourceRange = {
      .aspectMask = m_format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1
    }
  };
  
  try {
    // Create image view RAII object
    m_imageView.emplace(m_device.context().device(), viewInfo);
  } catch (const vk::SystemError& e) {
    throw ZException(fmt::format("Failed to create image view: {}", e.what()));
  }
}

void ZVulkanTexture::uploadData(const void* data, size_t size)
{
  // Create a staging buffer
  auto stagingBuffer = m_device.createBuffer(
      size,
      vk::BufferUsageFlagBits::eTransferSrc,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  
  // Copy data to staging buffer
  stagingBuffer->copyData(data, size);
  
  // Transition image layout to transfer destination
  auto cmdBuffer = m_device.beginSingleTimeCommands();
  
  // Transition to transfer destination layout
  transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferDstOptimal);
  
  // Copy buffer to image
  vk::BufferImageCopy region{
    .bufferOffset = 0,
    .bufferRowLength = 0,
    .bufferImageHeight = 0,
    .imageSubresource = {
      .aspectMask = vk::ImageAspectFlagBits::eColor,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1
    },
    .imageOffset = {0, 0, 0},
    .imageExtent = {m_width, m_height, 1}
  };
  
  cmdBuffer.copyBufferToImage(stagingBuffer->buffer(), *m_image, vk::ImageLayout::eTransferDstOptimal, region);
  
  // Transition to shader read layout
  transitionLayout(cmdBuffer, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
  
  m_device.endSingleTimeCommands(cmdBuffer);
}

void ZVulkanTexture::downloadData(void* data, size_t size)
{
  if (!(m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible)) {
    // If not host visible, we need to use a staging buffer
    auto stagingBuffer = m_device.createBuffer(
        size,
        vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    
    // Transition image layout to transfer source
    auto cmdBuffer = m_device.beginSingleTimeCommands();
    
    // Transition to transfer source layout
    transitionLayout(cmdBuffer, m_currentLayout, vk::ImageLayout::eTransferSrcOptimal);
    
    // Copy image to buffer
    vk::BufferImageCopy region{
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1
      },
      .imageOffset = {0, 0, 0},
      .imageExtent = {m_width, m_height, 1}
    };
    
    cmdBuffer.copyImageToBuffer(*m_image, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer->buffer(), region);
    
    // Transition back to original layout
    transitionLayout(cmdBuffer, vk::ImageLayout::eTransferSrcOptimal, m_currentLayout);
    
    m_device.endSingleTimeCommands(cmdBuffer);
    
    // Copy data from staging buffer to host memory
    void* mappedMemory = stagingBuffer->m_bufferMemory->mapMemory(0, size);
    memcpy(data, mappedMemory, size);
    stagingBuffer->m_bufferMemory->unmapMemory();
  } else {
    // If host visible, we can map the memory directly
    void* mappedMemory = m_imageMemory->mapMemory(0, size);
    memcpy(data, mappedMemory, size);
    m_imageMemory->unmapMemory();
  }
}

void ZVulkanTexture::transitionLayout(vk::raii::CommandBuffer& cmdBuffer, 
                                    vk::ImageLayout oldLayout, 
                                    vk::ImageLayout newLayout)
{
  if (oldLayout == newLayout) {
    return;
  }
  
  // Declare access masks and pipeline stages
  vk::AccessFlags srcAccessMask;
  vk::AccessFlags dstAccessMask;
  vk::PipelineStageFlags srcStageMask;
  vk::PipelineStageFlags dstStageMask;
  
  // Determine access masks and pipeline stages based on layouts
  if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal) {
    srcAccessMask = vk::AccessFlagBits::eNone;
    dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStageMask = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    dstAccessMask = vk::AccessFlagBits::eShaderRead;
    srcStageMask = vk::PipelineStageFlagBits::eTransfer;
    dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
  } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal) {
    srcAccessMask = vk::AccessFlagBits::eNone;
    dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests;
  } else if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
    srcAccessMask = vk::AccessFlagBits::eNone;
    dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    srcStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
    dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
  } else if (oldLayout == vk::ImageLayout::eColorAttachmentOptimal && newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    dstAccessMask = vk::AccessFlagBits::eTransferRead;
    srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dstStageMask = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal && newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    srcAccessMask = vk::AccessFlagBits::eShaderRead;
    dstAccessMask = vk::AccessFlagBits::eTransferRead;
    srcStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    dstStageMask = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
    srcAccessMask = vk::AccessFlagBits::eTransferRead;
    dstAccessMask = vk::AccessFlagBits::eShaderRead;
    srcStageMask = vk::PipelineStageFlagBits::eTransfer;
    dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
  } else {
    throw ZException("Unsupported layout transition!");
  }
  
  // Create an image memory barrier using C++20 designated initializers
  const vk::ImageMemoryBarrier barrier{
    .srcAccessMask = srcAccessMask,
    .dstAccessMask = dstAccessMask,
    .oldLayout = oldLayout,
    .newLayout = newLayout,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = *m_image,
    .subresourceRange = {
      .aspectMask = m_format == vk::Format::eD32Sfloat ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1
    }
  };
  
  // Apply the pipeline barrier using the RAII command buffer
  cmdBuffer.pipelineBarrier(
    srcStageMask,
    dstStageMask,
    vk::DependencyFlags{},
    {},  // No memory barriers
    {},  // No buffer memory barriers
    barrier  // Image memory barriers
  );
  
  // Update current layout
  m_currentLayout = newLayout;
}



// ZVulkanBuffer implementation
ZVulkanBuffer::ZVulkanBuffer(ZVulkanDevice& device, 
                            size_t size, 
                            vk::BufferUsageFlags usage, 
                            vk::MemoryPropertyFlags properties)
  : m_device(device)
  , m_size(size)
  , m_usage(usage)
  , m_memoryProperties(properties)
{
  createBuffer();
  LOG(INFO) << "ZVulkanBuffer created: " << size << " bytes";
}

ZVulkanBuffer::~ZVulkanBuffer()
{
  LOG(INFO) << "Destroying ZVulkanBuffer";
}

void ZVulkanBuffer::createBuffer()
{
  // Create buffer with C++20 aggregate initialization
  vk::BufferCreateInfo bufferInfo{
    .size = m_size,
    .usage = m_usage,
    .sharingMode = vk::SharingMode::eExclusive
  };
  
  try {
    // Create buffer RAII object
    m_buffer.emplace(m_device.context().device(), bufferInfo);
    
    // Get memory requirements using RAII object
    const vk::MemoryRequirements memRequirements = m_buffer->getMemoryRequirements();
    
    // Get memory properties using RAII object
    const vk::PhysicalDeviceMemoryProperties memProperties = 
        m_device.context().physicalDevice().getMemoryProperties();
    
    // Find a suitable memory type for the buffer
    uint32_t memoryTypeIndex = 0;
    bool foundSuitableMemory = false;
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((memRequirements.memoryTypeBits & (1 << i)) && 
          (memProperties.memoryTypes[i].propertyFlags & m_memoryProperties) == m_memoryProperties) {
        memoryTypeIndex = i;
        foundSuitableMemory = true;
        break;
      }
    }
    
    if (!foundSuitableMemory) {
      throw ZException("Failed to find suitable memory type for buffer");
    }
    
    // Allocate memory using C++20 style initialization
    vk::MemoryAllocateInfo allocInfo{
      .allocationSize = memRequirements.size,
      .memoryTypeIndex = memoryTypeIndex
    };
    
    // Create memory RAII object
    m_bufferMemory.emplace(m_device.context().device(), allocInfo);
    
    // Bind memory to buffer using RAII objects
    m_buffer->bindMemory(*m_bufferMemory, 0);
  } catch (const vk::SystemError& e) {
    throw ZException(fmt::format("Failed to create buffer: {}", e.what()));
  }
}

void ZVulkanBuffer::copyData(const void* data, size_t size)
{
  if (size > m_size) {
    throw ZException("Data size exceeds buffer size");
  }
  
  if (!(m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible)) {
    // If not host visible, we need to use a staging buffer
    auto stagingBuffer = m_device.createBuffer(
        size,
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    
    // Copy data to staging buffer
    stagingBuffer->copyData(data, size);
    
    // Begin command buffer recording using RAII
    auto cmdBuffer = m_device.beginSingleTimeCommands();
    
    // Create buffer copy info with C++20 designated initializers
    const vk::BufferCopy copyRegion{
      .srcOffset = 0,
      .dstOffset = 0,
      .size = size
    };
    
    // Copy buffer using RAII command buffer
    cmdBuffer.copyBuffer(stagingBuffer->buffer(), *m_buffer, copyRegion);
    
    // End command buffer and submit to queue
    m_device.endSingleTimeCommands(cmdBuffer);
  } else {
    // If host visible, we can map the memory directly
    void* mappedMemory = m_bufferMemory->mapMemory(0, size);
    memcpy(mappedMemory, data, size);
    m_bufferMemory->unmapMemory();
  }
}

void* ZVulkanBuffer::map(vk::DeviceSize offset, vk::DeviceSize size)
{
  if (!(m_memoryProperties & vk::MemoryPropertyFlagBits::eHostVisible)) {
    throw ZException("Attempting to map memory that is not host visible");
  }
  
  // Use RAII DeviceMemory object to map the memory with proper error handling
  try {
    return m_bufferMemory->mapMemory(offset, size, {});
  } catch (const vk::SystemError& e) {
    throw ZException(fmt::format("Failed to map buffer memory: {}", e.what()));
  }
}

void ZVulkanBuffer::unmap()
{
  if (m_bufferMemory) {
    // Use RAII DeviceMemory object to unmap the memory
    m_bufferMemory->unmapMemory();
  }
}

// ZVulkanDescriptorPool implementation
ZVulkanDescriptorPool::ZVulkanDescriptorPool(ZVulkanDevice& device)
  : m_device(device)
{
  vk::DescriptorPoolSize poolSize{
    .type = vk::DescriptorType::eUniformBuffer,
    .descriptorCount = 100 // Preallocate enough for most use cases
  };
  
  vk::DescriptorPoolCreateInfo poolInfo{
    .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
    .maxSets = 100,
    .poolSizeCount = 1,
    .pPoolSizes = &poolSize
  };
  
  m_descriptorPool.emplace(m_device.context().device(), poolInfo);
  LOG(INFO) << "ZVulkanDescriptorPool created";
}

ZVulkanDescriptorPool::~ZVulkanDescriptorPool()
{
  LOG(INFO) << "Destroying ZVulkanDescriptorPool";
}

vk::raii::DescriptorSet ZVulkanDescriptorPool::allocateDescriptorSet(vk::DescriptorSetLayout layout)
{
  vk::DescriptorSetAllocateInfo allocInfo{
    .descriptorPool = *m_descriptorPool,
    .descriptorSetCount = 1,
    .pSetLayouts = &layout
  };
  
  vk::raii::DescriptorSets descriptorSets(m_device.context().device(), allocInfo);
  LOG(INFO) << "Allocated descriptor set";
  return std::move(descriptorSets.front());
}

// ZVulkanDescriptorSet implementation
ZVulkanDescriptorSet::ZVulkanDescriptorSet(ZVulkanDevice& device, vk::raii::DescriptorSet&& descriptorSet)
  : m_device(device)
  , m_descriptorSet(std::move(descriptorSet))
{
  LOG(INFO) << "ZVulkanDescriptorSet created";
}

ZVulkanDescriptorSet::~ZVulkanDescriptorSet()
{
  LOG(INFO) << "Destroying ZVulkanDescriptorSet";
}

void ZVulkanDescriptorSet::updateUniformBuffer(uint32_t binding, ZVulkanBuffer& buffer)
{
  vk::DescriptorBufferInfo bufferInfo{
    .buffer = buffer.buffer(),
    .offset = 0,
    .range = buffer.size()
  };
  
  vk::WriteDescriptorSet descriptorWrite{
    .dstSet = *m_descriptorSet,
    .dstBinding = binding,
    .dstArrayElement = 0,
    .descriptorCount = 1,
    .descriptorType = vk::DescriptorType::eUniformBuffer,
    .pImageInfo = nullptr,
    .pBufferInfo = &bufferInfo,
    .pTexelBufferView = nullptr
  };
  
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  LOG(INFO) << "Updated uniform buffer descriptor at binding " << binding;
}

void ZVulkanDescriptorSet::updateTexture(uint32_t binding, ZVulkanTexture& texture, vk::Sampler sampler)
{
  vk::DescriptorImageInfo imageInfo{
    .sampler = sampler,
    .imageView = texture.imageView(),
    .imageLayout = texture.layout()
  };
  
  vk::WriteDescriptorSet descriptorWrite{
    .dstSet = *m_descriptorSet,
    .dstBinding = binding,
    .dstArrayElement = 0,
    .descriptorCount = 1,
    .descriptorType = vk::DescriptorType::eCombinedImageSampler,
    .pImageInfo = &imageInfo,
    .pBufferInfo = nullptr,
    .pTexelBufferView = nullptr
  };
  
  std::vector<vk::WriteDescriptorSet> descriptorWrites = {descriptorWrite};
  std::vector<vk::CopyDescriptorSet> descriptorCopies;
  m_device.context().device().updateDescriptorSets(descriptorWrites, descriptorCopies);
  LOG(INFO) << "Updated texture descriptor at binding " << binding;
}

// ZVulkanSwapChain implementation
ZVulkanSwapChain::ZVulkanSwapChain(ZVulkanDevice& device, uint32_t width, uint32_t height)
  : m_device(device)
  , m_width(width)
  , m_height(height)
{
  createAttachments();
  createSampler();
  createCommandBuffers();
  
  LOG(INFO) << "ZVulkanSwapChain created with size " << width << "x" << height;
}

ZVulkanSwapChain::~ZVulkanSwapChain()
{
  LOG(INFO) << "Destroying ZVulkanSwapChain";
}

void ZVulkanSwapChain::createAttachments()
{
  // Create an offscreen rendering target for simplicity
  m_colorAttachment = m_device.createTexture(
    m_width, 
    m_height, 
    vk::Format::eR8G8B8A8Unorm,
    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
    vk::MemoryPropertyFlagBits::eDeviceLocal
  );
  
  m_depthAttachment = m_device.createTexture(
    m_width, 
    m_height, 
    vk::Format::eD32Sfloat,
    vk::ImageUsageFlagBits::eDepthStencilAttachment,
    vk::MemoryPropertyFlagBits::eDeviceLocal
  );
}

void ZVulkanSwapChain::createSampler()
{
  vk::SamplerCreateInfo samplerInfo{
    .magFilter = vk::Filter::eLinear,
    .minFilter = vk::Filter::eLinear,
    .mipmapMode = vk::SamplerMipmapMode::eLinear,
    .addressModeU = vk::SamplerAddressMode::eRepeat,
    .addressModeV = vk::SamplerAddressMode::eRepeat,
    .addressModeW = vk::SamplerAddressMode::eRepeat,
    .mipLodBias = 0.0f,
    .anisotropyEnable = VK_TRUE,
    .maxAnisotropy = 16,
    .compareEnable = VK_FALSE,
    .compareOp = vk::CompareOp::eAlways,
    .minLod = 0.0f,
    .maxLod = 0.0f,
    .borderColor = vk::BorderColor::eIntOpaqueBlack
  };
  
  m_sampler.emplace(m_device.context().device(), samplerInfo);
  LOG(INFO) << "Sampler created successfully";
}

void ZVulkanSwapChain::createCommandBuffers()
{
  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  poolInfo.queueFamilyIndex = m_device.context().queueFamilyIndices().graphicsFamily.value();
  
  m_commandPool.emplace(m_device.context().device(), poolInfo);
  
  vk::CommandBufferAllocateInfo allocInfo;
  allocInfo.commandPool = *m_commandPool;
  allocInfo.level = vk::CommandBufferLevel::ePrimary;
  allocInfo.commandBufferCount = 1;
  
  m_commandBuffers.emplace(m_device.context().device(), allocInfo);
  
  vk::FenceCreateInfo fenceInfo;
  fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
  m_inFlightFence.emplace(m_device.context().device(), fenceInfo);
}

void ZVulkanSwapChain::resize(uint32_t width, uint32_t height)
{
  if (width == m_width && height == m_height) {
    return;
  }
  
  m_width = width;
  m_height = height;
  
  // Recreate resources
  createAttachments();
  
  LOG(INFO) << "Resized ZVulkanSwapChain to " << width << "x" << height;
}

vk::raii::CommandBuffer ZVulkanSwapChain::beginFrame(vk::ClearColorValue clearColor, vk::ClearDepthStencilValue clearDepthStencil)
{
  // Start recording
  vk::CommandBufferBeginInfo beginInfo;
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  
  (*m_commandBuffers)[m_currentBuffer].begin(beginInfo);
  
  // Set up the render pass for memory-to-memory rendering
  VkClearValue colorClearValue {};
  colorClearValue.color.float32[0] = clearColor.float32[0];
  colorClearValue.color.float32[1] = clearColor.float32[1];
  colorClearValue.color.float32[2] = clearColor.float32[2];
  colorClearValue.color.float32[3] = clearColor.float32[3];
  
  VkClearValue depthClearValue {};
  depthClearValue.depthStencil.depth = clearDepthStencil.depth;
  depthClearValue.depthStencil.stencil = clearDepthStencil.stencil;
  
  // Create rendering info structures
  std::array<vk::RenderingAttachmentInfo, 1> colorAttachments;
  colorAttachments[0].imageView = m_colorAttachment->imageView();
  colorAttachments[0].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachments[0].loadOp = vk::AttachmentLoadOp::eClear;
  colorAttachments[0].storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachments[0].clearValue = *reinterpret_cast<vk::ClearValue*>(&colorClearValue);
  
  vk::RenderingAttachmentInfo depthAttachment;
  depthAttachment.imageView = m_depthAttachment->imageView();
  depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
  depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
  depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  depthAttachment.clearValue = *reinterpret_cast<vk::ClearValue*>(&depthClearValue);
  
  vk::RenderingInfo renderingInfo;
  renderingInfo.renderArea = vk::Rect2D({0, 0}, {m_width, m_height});
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
  renderingInfo.pColorAttachments = colorAttachments.data();
  renderingInfo.pDepthAttachment = &depthAttachment;
  
  // Begin rendering
  (*m_commandBuffers)[m_currentBuffer].beginRendering(renderingInfo);
  
  // Return the command buffer via move semantics to transfer ownership
  auto& cmdBuffer = (*m_commandBuffers)[m_currentBuffer]; 
  return std::move(cmdBuffer);
}

void ZVulkanSwapChain::endFrame(vk::raii::CommandBuffer& commandBuffer)
{
  commandBuffer.endRendering();
  commandBuffer.end();
  
  vk::SubmitInfo submitInfo;
  vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
  submitInfo.waitSemaphoreCount = 0;
  submitInfo.pWaitSemaphores = nullptr;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  vk::CommandBuffer cmdBuffers[] = {*commandBuffer};
  submitInfo.pCommandBuffers = cmdBuffers;
  
  m_device.context().graphicsQueue().submit(submitInfo, *m_inFlightFence);
  auto waitStatus = m_device.context().device().waitForFences({*m_inFlightFence}, VK_TRUE, UINT64_MAX);
  m_device.context().device().resetFences({*m_inFlightFence});
}

void ZVulkanSwapChain::copyToMemory(void* data, size_t size)
{
  // Create staging buffer
  auto stagingBuffer = m_device.createBuffer(
    size,
    vk::BufferUsageFlagBits::eTransferDst,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
  );
  
  // Start command buffer recording for copying image to buffer
  vk::CommandBufferBeginInfo beginInfo;
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  
  (*m_commandBuffers)[m_currentBuffer].begin(beginInfo);
  
  // Copy image to buffer
  vk::BufferImageCopy region;
  region.bufferOffset = 0;
  region.bufferRowLength = 0;  // Tightly packed
  region.bufferImageHeight = 0;  // Tightly packed
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = vk::Offset3D{0, 0, 0};
  region.imageExtent = vk::Extent3D{m_width, m_height, 1};
  
  (*m_commandBuffers)[m_currentBuffer].copyImageToBuffer(
    m_colorAttachment->image(),
    vk::ImageLayout::eTransferSrcOptimal,
    stagingBuffer->buffer(),
    region
  );
  
  (*m_commandBuffers)[m_currentBuffer].end();
  
  vk::SubmitInfo submitInfo;
  submitInfo.commandBufferCount = 1;
  vk::CommandBuffer cmdBuffers[] = {*(*m_commandBuffers)[m_currentBuffer]};
  submitInfo.pCommandBuffers = cmdBuffers;
  
  m_device.context().graphicsQueue().submit(submitInfo, *m_inFlightFence);
  m_device.context().device().waitForFences({*m_inFlightFence}, VK_TRUE, UINT64_MAX);
  m_device.context().device().resetFences({*m_inFlightFence});
  
  // Copy data from staging buffer to host memory using the map/unmap methods
  void* mappedMemory = stagingBuffer->map(0, size);
  memcpy(data, mappedMemory, size);
  stagingBuffer->unmap();
}

// Shader utilities namespace
namespace ZVulkanShaderUtils
{
  void compileGLSLToSPIRV(
      const std::string& glslSource,
      vk::ShaderStageFlagBits stage,
      std::vector<uint32_t>& spirvOut)
  {
    // Suppress unused parameter warnings
    (void)glslSource;
    (void)stage;
    
    // This is just a placeholder - in a real implementation this would call the glslang library
    // or another shader compiler library to compile GLSL to SPIR-V
    LOG(WARNING) << "GLSL to SPIR-V compilation not implemented - would use glslang or shaderc in production";
    
    // For demo purposes, we would return a dummy SPIR-V module
    spirvOut.clear();
  }
}

} // namespace nim