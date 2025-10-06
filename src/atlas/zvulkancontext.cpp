#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zexception.h"
#include "zlog.h"

#include <set>

DEFINE_bool(atlas_debug_vulkan, false, "Whether to enable Vulkan validation and debug utils");

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

// ZVulkanContext implementation
ZVulkanContext::ZVulkanContext()
{
  try {
    // Initialize Vulkan context
    m_context.emplace();
    // Because Vulkan 1.0 implementations may fail with VK_ERROR_INCOMPATIBLE_DRIVER, applications should determine the
    // version of Vulkan available before calling vkCreateInstance. If the vkGetInstanceProcAddr returns NULL for
    // vkEnumerateInstanceVersion, it is a Vulkan 1.0 implementation. Otherwise, the application can call
    // vkEnumerateInstanceVersion to determine the version of Vulkan.
    if (!m_context->getDispatcher()->vkEnumerateInstanceVersion) {
      throw ZException("Require Vulkan 1.1 support (vkEnumerateInstanceVersion is NULL)");
    }

    // Determine what API version is available
    uint32_t apiVersion = m_context->enumerateInstanceVersion();

    LOG(INFO) << fmt::format("Available Vulkan API version: {}.{}.{}",
                             VK_VERSION_MAJOR(apiVersion),
                             VK_VERSION_MINOR(apiVersion),
                             VK_VERSION_PATCH(apiVersion));

    createInstance();
    setupDebugMessenger();
    pickPhysicalDevice();
    createLogicalDevice();
    createCommandPool();
    LOG(INFO) << "ZVulkanContext initialized successfully";
  }
  catch (const vk::SystemError& e) {
    LOG(ERROR) << "Vulkan system error: " << e.what();
    throw ZException(fmt::format("Vulkan system error: {}", e.what()));
  }
  catch (const std::exception& e) {
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
  vk::ApplicationInfo appInfo{.pApplicationName = "Atlas",
                              .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                              .pEngineName = "Z3D Vulkan Engine",
                              .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                              .apiVersion = VK_MAKE_VERSION(1, 3, 0)};

  // Get available layers and extensions
  std::vector<vk::LayerProperties> availableLayers = m_context->enumerateInstanceLayerProperties();
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

  std::vector<vk::ExtensionProperties> availableExtensions = m_context->enumerateInstanceExtensionProperties();
  LOG(INFO) << fmt::format("Available Instance Extensions ({})", availableExtensions.size());
  for (const auto& extension : availableExtensions) {
    LOG(INFO) << fmt::format("  Extension Name: {} (Spec Version: {})",
                             extension.extensionName.data(),
                             extension.specVersion);
  }

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
    .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                   vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
    .pfnUserCallback = debugUtilsMessengerCallback};

  try {
    m_debugMessenger.emplace(*m_instance, createInfo);
    LOG(INFO) << "Debug messenger created successfully";
  }
  catch (const vk::SystemError& e) {
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

  // Go through the list of physical devices and select only those that are capable
  for (auto& physicalDevice : physicalDevices) {
    auto deviceProperties = physicalDevice.getProperties();
    auto memProperties = physicalDevice.getMemoryProperties();
    auto deviceExtensionProperties = physicalDevice.enumerateDeviceExtensionProperties();

    auto features2 = physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                 vk::PhysicalDeviceVulkan12Features,
                                                 vk::PhysicalDeviceVulkan13Features>();
    // const vk::PhysicalDeviceFeatures& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
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
    m_physicalDevices.push_back(physicalDevice);
    m_queueFamilyIndices = indices;

    if (!m_physicalDevices.empty()) {
      auto selectedProperties = m_physicalDevices[0].getProperties();
      LOG(INFO) << fmt::format("Selected physical device: {}", selectedProperties.deviceName.data());
      LOG(INFO) << fmt::format("  API Version: {}", versionToString(selectedProperties.apiVersion));
      LOG(INFO) << fmt::format("  Driver Version: {}", versionToString(selectedProperties.driverVersion));
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
  if (m_physicalDevices.empty()) {
    throw ZException("Attempted to create logical device without a physical device");
  }

  // Create device with a single queue
  float queuePriority = 1.0f;

  std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueQueueFamilies = {m_queueFamilyIndices.graphicsFamily.value(),
                                            m_queueFamilyIndices.presentFamily.value()};

  for (uint32_t queueFamily : uniqueQueueFamilies) {
    vk::DeviceQueueCreateInfo queueCreateInfo{.queueFamilyIndex = queueFamily,
                                              .queueCount = 1,
                                              .pQueuePriorities = &queuePriority};
    queueCreateInfos.push_back(queueCreateInfo);
  }

  // Specify required device features
  auto features2 = m_physicalDevices[0]
                     .getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDeviceVulkan13Features>();
  auto& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
  auto& physicalDeviceVulkan12Features = features2.get<vk::PhysicalDeviceVulkan12Features>();
  // auto& physicalDeviceVulkan13Features = features2.get<vk::PhysicalDeviceVulkan13Features>();

  // Setup enabled features
  vk::
    StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>
      enabledFeatures2;
  auto& enabledPhysicalDeviceFeatures2 = enabledFeatures2.get<vk::PhysicalDeviceFeatures2>();
  auto& enabledPhysicalDeviceVulkan12Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& enabledPhysicalDeviceVulkan13Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan13Features>();

  // Enable basic features
  enabledPhysicalDeviceFeatures2.features.samplerAnisotropy = physicalDeviceFeatures.samplerAnisotropy;
  enabledPhysicalDeviceFeatures2.features.fillModeNonSolid = physicalDeviceFeatures.fillModeNonSolid;

  if (FLAGS_atlas_debug_vulkan) {
    enabledPhysicalDeviceFeatures2.features.robustBufferAccess = physicalDeviceFeatures.robustBufferAccess;
  }

  // Get device properties to determine API version
  auto deviceProperties = m_physicalDevices[0].getProperties();

  // Enable required extensions
  std::vector<const char*> enabledExtensions;

  if (deviceProperties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    // Enable Vulkan 1.3 features
    enabledPhysicalDeviceVulkan13Features.dynamicRendering = true;
    enabledPhysicalDeviceVulkan13Features.synchronization2 = true;
    // Enable useful 1.2 features via the 1.2 struct in the chain when supported
    enabledPhysicalDeviceVulkan12Features.separateDepthStencilLayouts =
      physicalDeviceVulkan12Features.separateDepthStencilLayouts;
  } else {
    // For Vulkan 1.2, use extensions
    enabledPhysicalDeviceVulkan12Features.descriptorIndexing = true;
    enabledPhysicalDeviceVulkan12Features.bufferDeviceAddress = true;
    enabledPhysicalDeviceVulkan12Features.separateDepthStencilLayouts =
      physicalDeviceVulkan12Features.separateDepthStencilLayouts;
    enabledFeatures2.unlink<vk::PhysicalDeviceVulkan13Features>();

    auto deviceExtensionProperties = m_physicalDevices[0].enumerateDeviceExtensionProperties();
    addRequiredExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, enabledExtensions, deviceExtensionProperties, false);
    addRequiredExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, enabledExtensions, deviceExtensionProperties, false);
  }

  // Add platform-specific required extensions
#ifdef __APPLE__
  auto deviceExtensionProperties = m_physicalDevices[0].enumerateDeviceExtensionProperties();
  addRequiredExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, enabledExtensions, deviceExtensionProperties, true);
#endif

  // Create the logical device
  vk::DeviceCreateInfo deviceCreateInfo{
    .pNext = &enabledPhysicalDeviceFeatures2,
    .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
    .pQueueCreateInfos = queueCreateInfos.data(),
    .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
    .ppEnabledExtensionNames = enabledExtensions.data(),
  };

  m_device.emplace(m_physicalDevices[0], deviceCreateInfo);
  LOG(INFO) << "Logical device created successfully";

  // Get device queues
  m_graphicsQueue.emplace(*m_device, m_queueFamilyIndices.graphicsFamily.value(), 0);
  m_presentQueue.emplace(*m_device, m_queueFamilyIndices.presentFamily.value(), 0);
  LOG(INFO) << "Device queues retrieved successfully";
}

void ZVulkanContext::createCommandPool()
{
  vk::CommandPoolCreateInfo poolInfo{.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                     .queueFamilyIndex = m_queueFamilyIndices.graphicsFamily.value()};

  m_commandPool.emplace(*m_device, poolInfo);
  LOG(INFO) << "Command pool created successfully";
}

std::unique_ptr<ZVulkanDevice> ZVulkanContext::createDevice()
{
  return std::make_unique<ZVulkanDevice>(*this);
}

} // namespace nim
