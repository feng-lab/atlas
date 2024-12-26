#include "zvulkan.h"

#include "zlog.h"

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

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                           VkDebugUtilsMessageTypeFlagsEXT /*messageTypes*/,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                           void* /*pUserData*/)
{
  // Determine the appropriate log level based on message severity
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
    // For verbose messages, use VLOG(1)
    VLOG(1) << pCallbackData->pMessage;
    return VK_FALSE; // Return early since we already logged the verbose message
  }
  auto logLevel = google::GLOG_INFO;
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    logLevel = google::GLOG_WARNING;
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
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
    // instantiate a vk::raii::Context
    vk::raii::Context context;

    // Because Vulkan 1.0 implementations may fail with VK_ERROR_INCOMPATIBLE_DRIVER, applications should determine the
    // version of Vulkan available before calling vkCreateInstance. If the vkGetInstanceProcAddr returns NULL for
    // vkEnumerateInstanceVersion, it is a Vulkan 1.0 implementation. Otherwise, the application can call
    // vkEnumerateInstanceVersion to determine the version of Vulkan.
    if (!context.getDispatcher()->vkEnumerateInstanceVersion) {
      throw ZException("Require Vulkan 1.1 support (vkEnumerateInstanceVersion is NULL)");
    }

    // Determine what API version is available
    uint32_t apiVersion = context.enumerateInstanceVersion();

    LOG(INFO) << fmt::format("Available Vulkan API version: {}.{}.{}",
                             VK_VERSION_MAJOR(apiVersion),
                             VK_VERSION_MINOR(apiVersion),
                             VK_VERSION_PATCH(apiVersion));

    auto instance = createVulkanInstance(context);

    vk::raii::DebugUtilsMessengerEXT debugUtilsMessenger(nullptr);
    if (FLAGS_atlas_debug_vulkan) {
      // Set up debug messenger
      vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = debugUtilsMessengerCallback};

      try {
        debugUtilsMessenger = vk::raii::DebugUtilsMessengerEXT(instance, debugCreateInfo);
        LOG(INFO) << "Debug messenger created successfully";
      }
      catch (const vk::SystemError& e) {
        LOG(ERROR) << fmt::format("Failed to create debug messenger: {}", e.what());
        // Continue without debug messenger
      }
    }

    // enumerate the physicalDevices
    vk::raii::PhysicalDevices physicalDevices(instance);

    if (physicalDevices.empty()) {
      LOG(WARNING) << "No Vulkan-compatible devices found";
    }

    // Go through the list of physical devices and select only those that are capable
    std::vector<vk::raii::PhysicalDevice> desiredPhysicalDevices;

    for (const auto& physicalDevice : physicalDevices) {
      auto deviceProperties = physicalDevice.getProperties();
      auto memProperties = physicalDevice.getMemoryProperties();
      auto deviceExtensionProperties = physicalDevice.enumerateDeviceExtensionProperties();

      auto features2 = physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                   vk::PhysicalDeviceVulkan12Features,
                                                   vk::PhysicalDeviceVulkan13Features>();
      const vk::PhysicalDeviceFeatures& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
      const vk::PhysicalDeviceVulkan12Features& physicalDeviceVulkan12Features =
        features2.get<vk::PhysicalDeviceVulkan12Features>();
      const vk::PhysicalDeviceVulkan13Features& physicalDeviceVulkan13Features =
        features2.get<vk::PhysicalDeviceVulkan13Features>();

      // Calculate dedicated GPU memory
      vk::DeviceSize dedicatedMemory = 0;
      for (uint32_t i = 0; i < memProperties.memoryHeapCount; i++) {
        if (memProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
          dedicatedMemory += memProperties.memoryHeaps[i].size;
        }
      }

      LOG(INFO) << fmt::format("Device Name:          {}", deviceProperties.deviceName.data());
      LOG(INFO) << fmt::format("API Version:          {} ({})",
                               versionToString(deviceProperties.apiVersion),
                               deviceProperties.apiVersion);
      LOG(INFO) << fmt::format("Driver Version:       {} ({})",
                               versionToString(deviceProperties.driverVersion),
                               deviceProperties.driverVersion);
      LOG(INFO) << fmt::format("Device ID:            0x{:04x}", deviceProperties.deviceID);
      LOG(INFO) << fmt::format("Device Type:          {}", vk::to_string(deviceProperties.deviceType));
      LOG(INFO) << fmt::format("Dedicated GPU Memory: {} MB", dedicatedMemory / (1024 * 1024));
      if (VLOG_IS_ON(1)) {
        VLOG(1) << fmt::format("Vendor ID:            0x{:04x}", deviceProperties.vendorID);
        VLOG(1) << fmt::format("Pipeline Cache UUID:  {}", uuidToString(deviceProperties.pipelineCacheUUID));
        VLOG(1) << "Supported Device Extensions: ";
        for (const auto& ext : deviceExtensionProperties) {
          VLOG(1) << fmt::format("  - {} (version {})", ext.extensionName.data(), ext.specVersion);
        }
        logVulkan12Features(physicalDeviceVulkan12Features);
        if (deviceProperties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
          logVulkan13Features(physicalDeviceVulkan13Features);
        }
      }

      LOG(INFO) << "-------------------------";

      std::vector<const char*> enabledDeviceExtensions;
      vk::StructureChain<vk::PhysicalDeviceFeatures2,
                         vk::PhysicalDeviceVulkan12Features,
                         vk::PhysicalDeviceVulkan13Features>
        enabledFeatures2;
      auto& enabledPhysicalDeviceFeatures2 = enabledFeatures2.get<vk::PhysicalDeviceFeatures2>();
      auto& enabledPhysicalDeviceVulkan12Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan12Features>();
      auto& enabledPhysicalDeviceVulkan13Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan13Features>();
      enabledPhysicalDeviceFeatures2.features = physicalDeviceFeatures;
      // disable some features in release mode
      if (!FLAGS_atlas_debug_vulkan) {
        enabledPhysicalDeviceFeatures2.features.robustBufferAccess = false;
      }

      if (deviceProperties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
        if (!physicalDeviceVulkan13Features.dynamicRendering) {
          LOG(WARNING) << "vulkan 1.3 dynamicRendering not supported, skip current device";
          continue;
        }
        if (!physicalDeviceVulkan13Features.synchronization2) {
          LOG(WARNING) << "vulkan 1.3 synchronization2 not supported, skip current device";
          continue;
        }
        enabledPhysicalDeviceVulkan13Features.dynamicRendering = true;
        enabledPhysicalDeviceVulkan13Features.synchronization2 = true;
        desiredPhysicalDevices.push_back(physicalDevice);
      } else if (deviceProperties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
        if (!physicalDeviceVulkan12Features.descriptorIndexing) {
          LOG(WARNING) << "vulkan 1.2 descriptorIndexing not supported, skip current device";
          continue;
        }
        if (!physicalDeviceVulkan12Features.bufferDeviceAddress) {
          LOG(WARNING) << "vulkan 1.2 bufferDeviceAddress not supported, skip current device";
          continue;
        }
        if (!isExtensionAvailable(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, deviceExtensionProperties)) {
          LOG(WARNING) << "vulkan 1.2 dynamicRendering extension not supported, skip current device";
          continue;
        }
        if (!isExtensionAvailable(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, deviceExtensionProperties)) {
          LOG(WARNING) << "vulkan 1.2 synchronization2 extension not supported, skip current device";
          continue;
        }
        enabledPhysicalDeviceVulkan12Features.descriptorIndexing = true;
        enabledPhysicalDeviceVulkan12Features.bufferDeviceAddress = true;
        enabledFeatures2.unlink<vk::PhysicalDeviceVulkan13Features>();
        addRequiredExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                             enabledDeviceExtensions,
                             deviceExtensionProperties,
                             false);
        addRequiredExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                             enabledDeviceExtensions,
                             deviceExtensionProperties,
                             false);
        desiredPhysicalDevices.push_back(physicalDevice);
      } else {
        LOG(WARNING) << "require at least vulkan 1.2, skip current device";
        continue;
      }

#ifdef __APPLE__
      addRequiredExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
                           enabledDeviceExtensions,
                           deviceExtensionProperties,
                           true);
#endif

      {
        // get the QueueFamilyProperties of the first PhysicalDevice
        auto queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

        // get the first index into queueFamiliyProperties which supports graphics
        auto propertyIterator = std::ranges::find_if(queueFamilyProperties, [](const vk::QueueFamilyProperties& qfp) {
          return static_cast<bool>(qfp.queueFlags & vk::QueueFlagBits::eGraphics);
        });
        CHECK(propertyIterator != queueFamilyProperties.end());
        auto graphicsQueueFamilyIndex =
          static_cast<uint32_t>(std::distance(queueFamilyProperties.begin(), propertyIterator));

        // create a Device
        float queuePriority = 0.0f;
        vk::DeviceQueueCreateInfo deviceQueueCreateInfo{.queueFamilyIndex = graphicsQueueFamilyIndex,
                                                        .queueCount = 1,
                                                        .pQueuePriorities = &queuePriority};

        vk::DeviceCreateInfo deviceCreateInfo{
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos = &deviceQueueCreateInfo,
          .enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size()),
          .ppEnabledExtensionNames = enabledDeviceExtensions.empty() ? nullptr : enabledDeviceExtensions.data()};

        // https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#features
        // Fine-grained features used by a logical device must be enabled at VkDevice creation time.
        // If an application wishes to enable all features supported by a device, it can simply pass in the
        // VkPhysicalDeviceFeatures structure that was previously returned by vkGetPhysicalDeviceFeatures. To disable an
        // individual feature, the application can set the desired member to VK_FALSE in the same structure. Setting
        // pEnabledFeatures to NULL and not including a VkPhysicalDeviceFeatures2 in the pNext chain of
        // VkDeviceCreateInfo is equivalent to setting all members of the structure to VK_FALSE.
        deviceCreateInfo.pNext = &enabledPhysicalDeviceFeatures2;

        vk::raii::Device device(physicalDevice, deviceCreateInfo);

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
      LOG(ERROR) << "Could not find suitable vulkan device";
    }
    LOG(INFO) << "Number of devices: " << desiredPhysicalDevices.size();
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "vulkan error: " << e.what();
  }
  catch (const ZException& e) {
    LOG(ERROR) << "error: " << e.what();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "std error: " << e.what();
  }
}

} // namespace nim