#include "zvulkancontext.h"
#include "zvulkandevice.h"
#include "zvulkanuniforms.h"
#include "zexception.h"
#include "zlog.h"

#include <set>
#include <algorithm>

DEFINE_bool(atlas_debug_vulkan, false, "Whether to enable Vulkan validation and debug utils");
DEFINE_int32(atlas_vk_device_index, -1, "Preferred Vulkan physical device index (sorted); -1 for auto");

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

std::string logVulkan10Features(const vk::PhysicalDeviceFeatures& features)
{
  std::string s;
  fmt::format_to(std::back_inserter(s), "Vulkan 1.0 Core Features:\n");
  fmt::format_to(std::back_inserter(s), "  robustBufferAccess: {}\n", features.robustBufferAccess);
  fmt::format_to(std::back_inserter(s), "  fullDrawIndexUint32: {}\n", features.fullDrawIndexUint32);
  fmt::format_to(std::back_inserter(s), "  imageCubeArray: {}\n", features.imageCubeArray);
  fmt::format_to(std::back_inserter(s), "  independentBlend: {}\n", features.independentBlend);
  fmt::format_to(std::back_inserter(s), "  geometryShader: {}\n", features.geometryShader);
  fmt::format_to(std::back_inserter(s), "  tessellationShader: {}\n", features.tessellationShader);
  fmt::format_to(std::back_inserter(s), "  sampleRateShading: {}\n", features.sampleRateShading);
  fmt::format_to(std::back_inserter(s), "  dualSrcBlend: {}\n", features.dualSrcBlend);
  fmt::format_to(std::back_inserter(s), "  logicOp: {}\n", features.logicOp);
  fmt::format_to(std::back_inserter(s), "  multiDrawIndirect: {}\n", features.multiDrawIndirect);
  fmt::format_to(std::back_inserter(s), "  drawIndirectFirstInstance: {}\n", features.drawIndirectFirstInstance);
  fmt::format_to(std::back_inserter(s), "  depthClamp: {}\n", features.depthClamp);
  fmt::format_to(std::back_inserter(s), "  depthBiasClamp: {}\n", features.depthBiasClamp);
  fmt::format_to(std::back_inserter(s), "  fillModeNonSolid: {}\n", features.fillModeNonSolid);
  fmt::format_to(std::back_inserter(s), "  depthBounds: {}\n", features.depthBounds);
  fmt::format_to(std::back_inserter(s), "  wideLines: {}\n", features.wideLines);
  fmt::format_to(std::back_inserter(s), "  largePoints: {}\n", features.largePoints);
  fmt::format_to(std::back_inserter(s), "  alphaToOne: {}\n", features.alphaToOne);
  fmt::format_to(std::back_inserter(s), "  multiViewport: {}\n", features.multiViewport);
  fmt::format_to(std::back_inserter(s), "  samplerAnisotropy: {}\n", features.samplerAnisotropy);
  fmt::format_to(std::back_inserter(s), "  textureCompressionETC2: {}\n", features.textureCompressionETC2);
  fmt::format_to(std::back_inserter(s), "  textureCompressionASTC_LDR: {}\n", features.textureCompressionASTC_LDR);
  fmt::format_to(std::back_inserter(s), "  textureCompressionBC: {}\n", features.textureCompressionBC);
  fmt::format_to(std::back_inserter(s), "  occlusionQueryPrecise: {}\n", features.occlusionQueryPrecise);
  fmt::format_to(std::back_inserter(s), "  pipelineStatisticsQuery: {}\n", features.pipelineStatisticsQuery);
  fmt::format_to(std::back_inserter(s),
                 "  vertexPipelineStoresAndAtomics: {}\n",
                 features.vertexPipelineStoresAndAtomics);
  fmt::format_to(std::back_inserter(s), "  fragmentStoresAndAtomics: {}\n", features.fragmentStoresAndAtomics);
  fmt::format_to(std::back_inserter(s),
                 "  shaderTessellationAndGeometryPointSize: {}\n",
                 features.shaderTessellationAndGeometryPointSize);
  fmt::format_to(std::back_inserter(s), "  shaderImageGatherExtended: {}\n", features.shaderImageGatherExtended);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageExtendedFormats: {}\n",
                 features.shaderStorageImageExtendedFormats);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageMultisample: {}\n",
                 features.shaderStorageImageMultisample);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageReadWithoutFormat: {}\n",
                 features.shaderStorageImageReadWithoutFormat);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageWriteWithoutFormat: {}\n",
                 features.shaderStorageImageWriteWithoutFormat);
  fmt::format_to(std::back_inserter(s),
                 "  shaderUniformBufferArrayDynamicIndexing: {}\n",
                 features.shaderUniformBufferArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderSampledImageArrayDynamicIndexing: {}\n",
                 features.shaderSampledImageArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageBufferArrayDynamicIndexing: {}\n",
                 features.shaderStorageBufferArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageArrayDynamicIndexing: {}\n",
                 features.shaderStorageImageArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s), "  shaderClipDistance: {}\n", features.shaderClipDistance);
  fmt::format_to(std::back_inserter(s), "  shaderCullDistance: {}\n", features.shaderCullDistance);
  fmt::format_to(std::back_inserter(s), "  shaderFloat64: {}\n", features.shaderFloat64);
  fmt::format_to(std::back_inserter(s), "  shaderInt64: {}\n", features.shaderInt64);
  fmt::format_to(std::back_inserter(s), "  shaderInt16: {}\n", features.shaderInt16);
  fmt::format_to(std::back_inserter(s), "  shaderResourceResidency: {}\n", features.shaderResourceResidency);
  fmt::format_to(std::back_inserter(s), "  shaderResourceMinLod: {}\n", features.shaderResourceMinLod);
  fmt::format_to(std::back_inserter(s), "  sparseBinding: {}\n", features.sparseBinding);
  fmt::format_to(std::back_inserter(s), "  sparseResidencyBuffer: {}\n", features.sparseResidencyBuffer);
  fmt::format_to(std::back_inserter(s), "  sparseResidencyImage2D: {}\n", features.sparseResidencyImage2D);
  fmt::format_to(std::back_inserter(s), "  sparseResidencyImage3D: {}\n", features.sparseResidencyImage3D);
  fmt::format_to(std::back_inserter(s), "  sparseResidency2Samples: {}\n", features.sparseResidency2Samples);
  fmt::format_to(std::back_inserter(s), "  sparseResidency4Samples: {}\n", features.sparseResidency4Samples);
  fmt::format_to(std::back_inserter(s), "  sparseResidency8Samples: {}\n", features.sparseResidency8Samples);
  fmt::format_to(std::back_inserter(s), "  sparseResidency16Samples: {}\n", features.sparseResidency16Samples);
  fmt::format_to(std::back_inserter(s), "  sparseResidencyAliased: {}\n", features.sparseResidencyAliased);
  fmt::format_to(std::back_inserter(s), "  variableMultisampleRate: {}\n", features.variableMultisampleRate);
  fmt::format_to(std::back_inserter(s), "  inheritedQueries: {}\n", features.inheritedQueries);
  return s;
}

std::string logVulkan11Features(const vk::PhysicalDeviceVulkan11Features& features)
{
  std::string s;
  fmt::format_to(std::back_inserter(s), "Vulkan 1.1 Features:\n");
  fmt::format_to(std::back_inserter(s), "  storageBuffer16BitAccess: {}\n", features.storageBuffer16BitAccess);
  fmt::format_to(std::back_inserter(s),
                 "  uniformAndStorageBuffer16BitAccess: {}\n",
                 features.uniformAndStorageBuffer16BitAccess);
  fmt::format_to(std::back_inserter(s), "  storagePushConstant16: {}\n", features.storagePushConstant16);
  fmt::format_to(std::back_inserter(s), "  storageInputOutput16: {}\n", features.storageInputOutput16);
  fmt::format_to(std::back_inserter(s), "  multiview: {}\n", features.multiview);
  fmt::format_to(std::back_inserter(s), "  multiviewGeometryShader: {}\n", features.multiviewGeometryShader);
  fmt::format_to(std::back_inserter(s), "  multiviewTessellationShader: {}\n", features.multiviewTessellationShader);
  fmt::format_to(std::back_inserter(s),
                 "  variablePointersStorageBuffer: {}\n",
                 features.variablePointersStorageBuffer);
  fmt::format_to(std::back_inserter(s), "  variablePointers: {}\n", features.variablePointers);
  fmt::format_to(std::back_inserter(s), "  protectedMemory: {}\n", features.protectedMemory);
  fmt::format_to(std::back_inserter(s), "  samplerYcbcrConversion: {}\n", features.samplerYcbcrConversion);
  fmt::format_to(std::back_inserter(s), "  shaderDrawParameters: {}\n", features.shaderDrawParameters);
  return s;
}

// Function to check if a layer is available
bool isLayerAvailable(const char* layerName, const std::vector<vk::LayerProperties>& availableLayers)
{
  CHECK(layerName);
  return std::ranges::find_if(availableLayers, [layerName](const vk::LayerProperties& layerProperties) {
           return strcmp(layerName, layerProperties.layerName) == 0;
         }) != availableLayers.end();
}

bool addRequiredLayers(const char* layerName,
                       std::vector<const char*>& enabledExtensions,
                       const std::vector<vk::LayerProperties>& availableLayers,
                       bool isOptional = false)
{
  CHECK(layerName);
  if (isLayerAvailable(layerName, availableLayers)) {
    enabledExtensions.push_back(layerName);
    return true;
  } else {
    if (!isOptional) {
      throw ZException(fmt::format("Required layer {} is not supported by the device.", layerName));
    } else {
      LOG(ERROR) << fmt::format("Optional layer {} is not supported by the device.", layerName);
    }
  }
  return false;
}

// Function to check if an extension is available
bool isExtensionAvailable(const char* extensionName, const std::vector<vk::ExtensionProperties>& availableExtensions)
{
  CHECK(extensionName);
  return std::ranges::find_if(availableExtensions, [extensionName](const vk::ExtensionProperties& extensionProperties) {
           return strcmp(extensionName, extensionProperties.extensionName) == 0;
         }) != availableExtensions.end();
}

bool addRequiredExtension(const char* extensionName,
                          std::vector<const char*>& enabledExtensions,
                          const std::vector<vk::ExtensionProperties>& availableExtensions,
                          bool isOptional = false)
{
  CHECK(extensionName);
  if (isExtensionAvailable(extensionName, availableExtensions)) {
    enabledExtensions.push_back(extensionName);
    return true;
  } else {
    if (!isOptional) {
      throw ZException(fmt::format("Required extension {} is not supported by the device.", extensionName));
    } else {
      LOG(ERROR) << fmt::format("Optional extension {} is not supported by the device.", extensionName);
    }
  }
  return false;
}

std::string logVulkan12Features(const vk::PhysicalDeviceVulkan12Features& features)
{
  std::string s;
  fmt::format_to(std::back_inserter(s), "Vulkan 1.2 Features:\n");
  fmt::format_to(std::back_inserter(s), "  samplerMirrorClampToEdge: {}\n", features.samplerMirrorClampToEdge);
  fmt::format_to(std::back_inserter(s), "  drawIndirectCount: {}\n", features.drawIndirectCount);
  fmt::format_to(std::back_inserter(s), "  storageBuffer8BitAccess: {}\n", features.storageBuffer8BitAccess);
  fmt::format_to(std::back_inserter(s),
                 "  uniformAndStorageBuffer8BitAccess: {}\n",
                 features.uniformAndStorageBuffer8BitAccess);
  fmt::format_to(std::back_inserter(s), "  storagePushConstant8: {}\n", features.storagePushConstant8);
  fmt::format_to(std::back_inserter(s), "  shaderBufferInt64Atomics: {}\n", features.shaderBufferInt64Atomics);
  fmt::format_to(std::back_inserter(s), "  shaderSharedInt64Atomics: {}\n", features.shaderSharedInt64Atomics);
  fmt::format_to(std::back_inserter(s), "  shaderFloat16: {}\n", features.shaderFloat16);
  fmt::format_to(std::back_inserter(s), "  shaderInt8: {}\n", features.shaderInt8);
  fmt::format_to(std::back_inserter(s), "  descriptorIndexing: {}\n", features.descriptorIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderInputAttachmentArrayDynamicIndexing: {}\n",
                 features.shaderInputAttachmentArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderUniformTexelBufferArrayDynamicIndexing: {}\n",
                 features.shaderUniformTexelBufferArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageTexelBufferArrayDynamicIndexing: {}\n",
                 features.shaderStorageTexelBufferArrayDynamicIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderUniformBufferArrayNonUniformIndexing: {}\n",
                 features.shaderUniformBufferArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderSampledImageArrayNonUniformIndexing: {}\n",
                 features.shaderSampledImageArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageBufferArrayNonUniformIndexing: {}\n",
                 features.shaderStorageBufferArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageImageArrayNonUniformIndexing: {}\n",
                 features.shaderStorageImageArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderInputAttachmentArrayNonUniformIndexing: {}\n",
                 features.shaderInputAttachmentArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderUniformTexelBufferArrayNonUniformIndexing: {}\n",
                 features.shaderUniformTexelBufferArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  shaderStorageTexelBufferArrayNonUniformIndexing: {}\n",
                 features.shaderStorageTexelBufferArrayNonUniformIndexing);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingUniformBufferUpdateAfterBind: {}\n",
                 features.descriptorBindingUniformBufferUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingSampledImageUpdateAfterBind: {}\n",
                 features.descriptorBindingSampledImageUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingStorageImageUpdateAfterBind: {}\n",
                 features.descriptorBindingStorageImageUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingStorageBufferUpdateAfterBind: {}\n",
                 features.descriptorBindingStorageBufferUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingUniformTexelBufferUpdateAfterBind: {}\n",
                 features.descriptorBindingUniformTexelBufferUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingStorageTexelBufferUpdateAfterBind: {}\n",
                 features.descriptorBindingStorageTexelBufferUpdateAfterBind);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingUpdateUnusedWhilePending: {}\n",
                 features.descriptorBindingUpdateUnusedWhilePending);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingPartiallyBound: {}\n",
                 features.descriptorBindingPartiallyBound);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingVariableDescriptorCount: {}\n",
                 features.descriptorBindingVariableDescriptorCount);
  fmt::format_to(std::back_inserter(s), "  runtimeDescriptorArray: {}\n", features.runtimeDescriptorArray);
  fmt::format_to(std::back_inserter(s), "  samplerFilterMinmax: {}\n", features.samplerFilterMinmax);
  fmt::format_to(std::back_inserter(s), "  scalarBlockLayout: {}\n", features.scalarBlockLayout);
  fmt::format_to(std::back_inserter(s), "  imagelessFramebuffer: {}\n", features.imagelessFramebuffer);
  fmt::format_to(std::back_inserter(s), "  uniformBufferStandardLayout: {}\n", features.uniformBufferStandardLayout);
  fmt::format_to(std::back_inserter(s), "  shaderSubgroupExtendedTypes: {}\n", features.shaderSubgroupExtendedTypes);
  fmt::format_to(std::back_inserter(s), "  separateDepthStencilLayouts: {}\n", features.separateDepthStencilLayouts);
  fmt::format_to(std::back_inserter(s), "  hostQueryReset: {}\n", features.hostQueryReset);
  fmt::format_to(std::back_inserter(s), "  timelineSemaphore: {}\n", features.timelineSemaphore);
  fmt::format_to(std::back_inserter(s), "  bufferDeviceAddress: {}\n", features.bufferDeviceAddress);
  fmt::format_to(std::back_inserter(s),
                 "  bufferDeviceAddressCaptureReplay: {}\n",
                 features.bufferDeviceAddressCaptureReplay);
  fmt::format_to(std::back_inserter(s),
                 "  bufferDeviceAddressMultiDevice: {}\n",
                 features.bufferDeviceAddressMultiDevice);
  fmt::format_to(std::back_inserter(s), "  vulkanMemoryModel: {}\n", features.vulkanMemoryModel);
  fmt::format_to(std::back_inserter(s), "  vulkanMemoryModelDeviceScope: {}\n", features.vulkanMemoryModelDeviceScope);
  fmt::format_to(std::back_inserter(s),
                 "  vulkanMemoryModelAvailabilityVisibilityChains: {}\n",
                 features.vulkanMemoryModelAvailabilityVisibilityChains);
  fmt::format_to(std::back_inserter(s), "  shaderOutputViewportIndex: {}\n", features.shaderOutputViewportIndex);
  fmt::format_to(std::back_inserter(s), "  shaderOutputLayer: {}\n", features.shaderOutputLayer);
  fmt::format_to(std::back_inserter(s), "  subgroupBroadcastDynamicId: {}\n", features.subgroupBroadcastDynamicId);
  return s;
}

std::string logVulkan13Features(const vk::PhysicalDeviceVulkan13Features& features)
{
  std::string s;
  fmt::format_to(std::back_inserter(s), "Vulkan 1.3 Features:\n");
  fmt::format_to(std::back_inserter(s), "  robustImageAccess: {}\n", features.robustImageAccess);
  fmt::format_to(std::back_inserter(s), "  inlineUniformBlock: {}\n", features.inlineUniformBlock);
  fmt::format_to(std::back_inserter(s),
                 "  descriptorBindingInlineUniformBlockUpdateAfterBind: {}\n",
                 features.descriptorBindingInlineUniformBlockUpdateAfterBind);
  fmt::format_to(std::back_inserter(s), "  pipelineCreationCacheControl: {}\n", features.pipelineCreationCacheControl);
  fmt::format_to(std::back_inserter(s), "  privateData: {}\n", features.privateData);
  fmt::format_to(std::back_inserter(s),
                 "  shaderDemoteToHelperInvocation: {}\n",
                 features.shaderDemoteToHelperInvocation);
  fmt::format_to(std::back_inserter(s), "  shaderTerminateInvocation: {}\n", features.shaderTerminateInvocation);
  fmt::format_to(std::back_inserter(s), "  subgroupSizeControl: {}\n", features.subgroupSizeControl);
  fmt::format_to(std::back_inserter(s), "  computeFullSubgroups: {}\n", features.computeFullSubgroups);
  fmt::format_to(std::back_inserter(s), "  synchronization2: {}\n", features.synchronization2);
  fmt::format_to(std::back_inserter(s), "  textureCompressionASTC_HDR: {}\n", features.textureCompressionASTC_HDR);
  fmt::format_to(std::back_inserter(s),
                 "  shaderZeroInitializeWorkgroupMemory: {}\n",
                 features.shaderZeroInitializeWorkgroupMemory);
  fmt::format_to(std::back_inserter(s), "  dynamicRendering: {}\n", features.dynamicRendering);
  fmt::format_to(std::back_inserter(s), "  shaderIntegerDotProduct: {}\n", features.shaderIntegerDotProduct);
  fmt::format_to(std::back_inserter(s), "  maintenance4: {}\n", features.maintenance4);
  return s;
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

    // Enforce minimum Vulkan 1.3 at the instance level
    if (apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      throw ZException("Require Vulkan 1.3 (enumerated instance version < 1.3)");
    }

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

void ZVulkanContext::logGpuInfo() const
{
  if (m_physicalDevices.empty()) {
    LOG(WARNING) << "Vulkan GPU info requested but no physical devices are available";
    return;
  }
  std::string summary;
  fmt::format_to(std::back_inserter(summary), "Vulkan GPUs: {} device(s)\n", m_physicalDevices.size());
  for (size_t i = 0; i < m_physicalDevices.size(); ++i) {
    const auto& pd = m_physicalDevices[i];
    auto props = pd.getProperties();
    auto memProps = pd.getMemoryProperties();
    uint64_t dedicatedMemoryBytes = 0;
    for (uint32_t h = 0; h < memProps.memoryHeapCount; ++h) {
      if (memProps.memoryHeaps[h].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
        dedicatedMemoryBytes += memProps.memoryHeaps[h].size;
      }
    }
    fmt::format_to(std::back_inserter(summary),
                   "[{}] {}{}\n",
                   i,
                   props.deviceName.data(),
                   (i == m_selectedDeviceIndex ? "  (selected)" : ""));
    fmt::format_to(std::back_inserter(summary),
                   "     API Version:              {} (0x{:08x})\n",
                   versionToString(props.apiVersion),
                   props.apiVersion);
    fmt::format_to(std::back_inserter(summary),
                   "     Driver Version:           {} (0x{:08x})\n",
                   versionToString(props.driverVersion),
                   props.driverVersion);
    fmt::format_to(std::back_inserter(summary), "     Vendor ID:                0x{:04x}\n", props.vendorID);
    fmt::format_to(std::back_inserter(summary), "     Device ID:                0x{:04x}\n", props.deviceID);
    fmt::format_to(std::back_inserter(summary), "     Device Type:              {}\n", vk::to_string(props.deviceType));
    fmt::format_to(std::back_inserter(summary),
                   "     Dedicated GPU Memory:     {} MB\n",
                   dedicatedMemoryBytes / (1024 * 1024));

    // Limits/features summary for every device
    auto features2 = pd.getFeatures2<vk::PhysicalDeviceFeatures2,
                                     vk::PhysicalDeviceVulkan11Features,
                                     vk::PhysicalDeviceVulkan12Features,
                                     vk::PhysicalDeviceVulkan13Features>();
    const auto& features = features2.get<vk::PhysicalDeviceFeatures2>().features;
    const auto& features11 = features2.get<vk::PhysicalDeviceVulkan11Features>();
    const auto& features12 = features2.get<vk::PhysicalDeviceVulkan12Features>();
    const auto& features13 = features2.get<vk::PhysicalDeviceVulkan13Features>();
    fmt::format_to(std::back_inserter(summary),
                   "     Max 2D Texture Size:      {}\n",
                   props.limits.maxImageDimension2D);
    fmt::format_to(std::back_inserter(summary),
                   "     Max 3D Texture Size:      {}\n",
                   props.limits.maxImageDimension3D);
    fmt::format_to(std::back_inserter(summary),
                   "     Max Array Layers:         {}\n",
                   props.limits.maxImageArrayLayers);
    fmt::format_to(std::back_inserter(summary),
                   "     Max Color Attachments:    {}\n",
                   props.limits.maxColorAttachments);
    fmt::format_to(std::back_inserter(summary),
                   "     Max Sampler Anisotropy:   {:.1f}\n",
                   features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f);
    fmt::format_to(
      std::back_inserter(summary),
      "     Features: anisotropy={} shaderClipDistance={} maxClipDistances={} independentBlend={} fragStoresAndAtomics={} drawIndirectCount={} dynamicRendering={} synchronization2={}\n",
      features.samplerAnisotropy == VK_TRUE,
      features.shaderClipDistance == VK_TRUE,
      props.limits.maxClipDistances,
      features.independentBlend == VK_TRUE,
      features.fragmentStoresAndAtomics == VK_TRUE,
      features12.drawIndirectCount == VK_TRUE,
      features13.dynamicRendering == VK_TRUE,
      features13.synchronization2 == VK_TRUE);

    // Per-device details: extensions and full Vulkan 1.2/1.3 features
    auto deviceExtensionProperties = pd.enumerateDeviceExtensionProperties();
    fmt::format_to(std::back_inserter(summary),
                   "     Supported Device Extensions ({}):\n",
                   deviceExtensionProperties.size());
    for (const auto& ext : deviceExtensionProperties) {
      fmt::format_to(std::back_inserter(summary),
                     "       - {} (version {})\n",
                     ext.extensionName.data(),
                     ext.specVersion);
    }
    fmt::format_to(std::back_inserter(summary), "{}", logVulkan10Features(features));
    fmt::format_to(std::back_inserter(summary), "{}", logVulkan11Features(features11));
    fmt::format_to(std::back_inserter(summary), "{}", logVulkan12Features(features12));
    if (props.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      fmt::format_to(std::back_inserter(summary), "{}", logVulkan13Features(features13));
    }
    fmt::format_to(std::back_inserter(summary), "\n");
  }
  LOG(INFO) << summary;
}

bool ZVulkanContext::setSelectedDeviceIndex(size_t index)
{
  if (index >= m_physicalDevices.size()) {
    LOG(ERROR) << fmt::format("Requested Vulkan device index {} out of range ({} devices)",
                              index,
                              m_physicalDevices.size());
    return false;
  }
  if (index == m_selectedDeviceIndex) {
    VLOG(1) << fmt::format("Vulkan device index {} already selected; no changes", index);
    return false;
  }

  // Validate suitability before switching
  auto& pd = m_physicalDevices[index];
  if (!checkDeviceExtensionSupport(pd)) {
    LOG(ERROR) << fmt::format("Vulkan device {} missing required extensions", index);
    return false;
  }
  auto queues = findQueueFamilies(pd);
  if (!queues.isComplete()) {
    LOG(ERROR) << fmt::format("Vulkan device {} does not have required queue families", index);
    return false;
  }
  const auto props = pd.getProperties();
  if (props.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    LOG(ERROR) << fmt::format("Vulkan device {} does not support Vulkan 1.3", index);
    return false;
  }

  // Destroy logical device and dependent resources before switching
  m_commandPool.reset();
  m_graphicsQueue.reset();
  m_presentQueue.reset();
  m_device.reset();

  // Switch selection and queue families, and recreate device/queues/pool
  m_selectedDeviceIndex = index;
  m_queueFamilyIndices = queues;

  try {
    createLogicalDevice();
    createCommandPool();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << fmt::format("Failed to recreate Vulkan logical device for index {}: {}", index, e.what());
    return false;
  }

  const auto propsNew = m_physicalDevices[m_selectedDeviceIndex].getProperties();
  LOG(INFO) << fmt::format("Switched to Vulkan device [{}]: {}", m_selectedDeviceIndex, propsNew.deviceName.data());
  return true;
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
  {
    std::string layersMsg;
    fmt::format_to(std::back_inserter(layersMsg), "Available Instance Layers ({})\n", availableLayers.size());
    for (const auto& layer : availableLayers) {
      fmt::format_to(std::back_inserter(layersMsg),
                     "  Layer Name: {} (Description: {}, Spec Version: {}.{}.{}, Implementation Version: {})\n",
                     layer.layerName.data(),
                     layer.description.data(),
                     VK_VERSION_MAJOR(layer.specVersion),
                     VK_VERSION_MINOR(layer.specVersion),
                     VK_VERSION_PATCH(layer.specVersion),
                     layer.implementationVersion);
    }
    LOG(INFO) << layersMsg;
  }

  std::vector<vk::ExtensionProperties> availableExtensions = m_context->enumerateInstanceExtensionProperties();
  {
    std::string extMsg;
    fmt::format_to(std::back_inserter(extMsg), "Available Instance Extensions ({})\n", availableExtensions.size());
    for (const auto& extension : availableExtensions) {
      fmt::format_to(std::back_inserter(extMsg),
                     "  Extension Name: {} (Spec Version: {})\n",
                     extension.extensionName.data(),
                     extension.specVersion);
    }
    LOG(INFO) << extMsg;
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

namespace {
int deviceTypeRank(vk::PhysicalDeviceType type)
{
  switch (type) {
    case vk::PhysicalDeviceType::eDiscreteGpu:
      return 4;
    case vk::PhysicalDeviceType::eIntegratedGpu:
      return 3;
    case vk::PhysicalDeviceType::eVirtualGpu:
      return 2;
    case vk::PhysicalDeviceType::eCpu:
      return 1;
    default:
      return 0;
  }
}
} // namespace

void ZVulkanContext::pickPhysicalDevice()
{
  vk::raii::PhysicalDevices enumerated(*m_instance);
  if (enumerated.empty()) {
    throw ZException("No Vulkan-compatible devices found");
  }
  struct DeviceInfo
  {
    vk::raii::PhysicalDevice device{nullptr};
    QueueFamilyIndices queues{};
    vk::PhysicalDeviceProperties props{};
    uint64_t dedicatedBytes = 0;
    bool suitable = false;
  };

  std::vector<DeviceInfo> infos;
  infos.reserve(enumerated.size());

  // Collect information and suitability for each device
  for (auto& pd : enumerated) {
    DeviceInfo info;
    info.device = std::move(pd);
    info.props = info.device.getProperties();
    auto mem = info.device.getMemoryProperties();
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
      if (mem.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
        info.dedicatedBytes += mem.memoryHeaps[i].size;
      }
    }

    bool apiOk = (info.props.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0));
    bool extOk = checkDeviceExtensionSupport(info.device);
    info.queues = findQueueFamilies(info.device);
    bool qOk = info.queues.isComplete();
    info.suitable = apiOk && extOk && qOk;

    infos.emplace_back(std::move(info));
  }

  // Sort by power: dedicated VRAM desc, then device type, then API version desc
  std::sort(infos.begin(), infos.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
    const int ra = deviceTypeRank(a.props.deviceType);
    const int rb = deviceTypeRank(b.props.deviceType);
    if (ra != rb) {
      return ra > rb; // Discrete > Integrated > Virtual > CPU
    }
    if (a.dedicatedBytes != b.dedicatedBytes) {
      return a.dedicatedBytes > b.dedicatedBytes; // then VRAM desc
    }
    return a.props.apiVersion > b.props.apiVersion; // then newer API
  });

  // Move sorted devices into member vector
  m_physicalDevices.clear();
  m_physicalDevices.reserve(infos.size());
  for (auto& di : infos) {
    m_physicalDevices.emplace_back(std::move(di.device));
  }

  // Pick the first suitable device as selected
  m_selectedDeviceIndex = 0;
  bool found = false;
  for (size_t i = 0; i < infos.size(); ++i) {
    if (infos[i].suitable) {
      m_selectedDeviceIndex = i;
      m_queueFamilyIndices = infos[i].queues;
      found = true;
      break;
    }
  }
  // Honor preferred index when explicitly requested
  if (FLAGS_atlas_vk_device_index >= 0 && static_cast<size_t>(FLAGS_atlas_vk_device_index) < infos.size()) {
    const size_t pref = static_cast<size_t>(FLAGS_atlas_vk_device_index);
    if (infos[pref].suitable) {
      m_selectedDeviceIndex = pref;
      m_queueFamilyIndices = infos[pref].queues;
      found = true;
    } else {
      LOG(WARNING) << fmt::format("Preferred Vulkan device index {} is not suitable; keeping auto-selected {}",
                                  pref,
                                  m_selectedDeviceIndex);
    }
  }
  if (!found) {
    throw ZException("Failed to find a suitable GPU");
  }

  // Log properties for all devices and mark selection
  LOG(INFO) << fmt::format("Found {} Vulkan device(s)", infos.size());
  for (size_t i = 0; i < infos.size(); ++i) {
    const auto& p = infos[i].props;
    LOG(INFO) << fmt::format("[{}] Device Name:          {}{}",
                             i,
                             p.deviceName.data(),
                             (i == m_selectedDeviceIndex ? "  (selected)" : ""));
    LOG(INFO) << fmt::format("      API Version:          {} (0x{:08x})", versionToString(p.apiVersion), p.apiVersion);
    LOG(INFO) << fmt::format("      Driver Version:       {} (0x{:08x})",
                             versionToString(p.driverVersion),
                             p.driverVersion);
    LOG(INFO) << fmt::format("      Vendor ID:            0x{:04x}", p.vendorID);
    LOG(INFO) << fmt::format("      Device ID:            0x{:04x}", p.deviceID);
    LOG(INFO) << fmt::format("      Device Type:          {}", vk::to_string(p.deviceType));
    LOG(INFO) << fmt::format("      Dedicated GPU Memory: {} MB", infos[i].dedicatedBytes / (1024 * 1024));
    LOG(INFO) << "-------------------------";
  }

  const auto selected = m_physicalDevices[m_selectedDeviceIndex].getProperties();
  LOG(INFO) << fmt::format("Selected physical device: {}", selected.deviceName.data());
  LOG(INFO) << fmt::format("  API Version: {}", versionToString(selected.apiVersion));
  LOG(INFO) << fmt::format("  Driver Version: {}", versionToString(selected.driverVersion));
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

  // No functional device extensions are required when targeting Vulkan 1.3 minimum.

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
  auto features2 = m_physicalDevices[m_selectedDeviceIndex]
                     .getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDeviceVulkan13Features,
                                   vk::PhysicalDeviceMaintenance7FeaturesKHR,
                                   vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();
  auto& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
  auto& physicalDeviceVulkan12Features = features2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& physicalDeviceVulkan13Features = features2.get<vk::PhysicalDeviceVulkan13Features>();
  auto& physicalDeviceMaintenance7Features = features2.get<vk::PhysicalDeviceMaintenance7FeaturesKHR>();
  auto& physicalDeviceNestedCommandBufferFeatures = features2.get<vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();

  // Setup enabled features
  vk::StructureChain<vk::PhysicalDeviceFeatures2,
                     vk::PhysicalDeviceVulkan12Features,
                     vk::PhysicalDeviceVulkan13Features,
                     vk::PhysicalDeviceMaintenance7FeaturesKHR,
                     vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>
    enabledFeatures2;
  auto& enabledPhysicalDeviceFeatures2 = enabledFeatures2.get<vk::PhysicalDeviceFeatures2>();
  auto& enabledPhysicalDeviceVulkan12Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& enabledPhysicalDeviceVulkan13Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan13Features>();
  auto& enabledPhysicalDeviceMaintenance7Features = enabledFeatures2.get<vk::PhysicalDeviceMaintenance7FeaturesKHR>();
  auto& enabledPhysicalDeviceNestedCommandBufferFeatures =
    enabledFeatures2.get<vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();

  // Clip planes are required for parity with the OpenGL backend's local/global
  // XYZ cuts. These use gl_ClipDistance in vertex shaders, so we must enable
  // the Vulkan core shaderClipDistance feature and validate the device limit.
  const auto properties = m_physicalDevices[m_selectedDeviceIndex].getProperties();
  if (physicalDeviceFeatures.shaderClipDistance != VK_TRUE) {
    throw ZException("Selected Vulkan device does not support shaderClipDistance (required for XYZ cut clip planes)");
  }
  if (properties.limits.maxClipDistances < kVulkanMaxClipDistances ||
      properties.limits.maxCombinedClipAndCullDistances < kVulkanMaxClipDistances) {
    throw ZException(fmt::format("Selected Vulkan device supports only {} clip distances (combined {}), but Atlas "
                                 "requires at least {} (extra planes are applied in the fragment shader)",
                                 properties.limits.maxClipDistances,
                                 properties.limits.maxCombinedClipAndCullDistances,
                                 kVulkanMaxClipDistances));
  }

  // Enable basic features
  enabledPhysicalDeviceFeatures2.features.samplerAnisotropy = physicalDeviceFeatures.samplerAnisotropy;
  enabledPhysicalDeviceFeatures2.features.fillModeNonSolid = physicalDeviceFeatures.fillModeNonSolid;
  // Enable independentBlend if supported to allow per-attachment blend state
  enabledPhysicalDeviceFeatures2.features.independentBlend = physicalDeviceFeatures.independentBlend;
  // Allow storage buffer/image writes in fragment shader when supported
  enabledPhysicalDeviceFeatures2.features.fragmentStoresAndAtomics = physicalDeviceFeatures.fragmentStoresAndAtomics;
  enabledPhysicalDeviceFeatures2.features.shaderClipDistance = VK_TRUE;

  if (FLAGS_atlas_debug_vulkan) {
    enabledPhysicalDeviceFeatures2.features.robustBufferAccess = physicalDeviceFeatures.robustBufferAccess;
  }

  // Enable required extensions
  std::vector<const char*> enabledExtensions;

  // Vulkan 1.3 is required, but individual features are still optional and must
  // be queried/enabled explicitly.
  if (physicalDeviceVulkan13Features.dynamicRendering != VK_TRUE) {
    throw ZException("Selected Vulkan device does not support dynamicRendering (required by Vulkan backend)");
  }
  if (physicalDeviceVulkan13Features.synchronization2 != VK_TRUE) {
    throw ZException("Selected Vulkan device does not support synchronization2 (required by Vulkan backend)");
  }
  enabledPhysicalDeviceVulkan13Features.dynamicRendering = true;
  enabledPhysicalDeviceVulkan13Features.synchronization2 = true;
  // Optional: enable only if supported by the device.
  enabledPhysicalDeviceVulkan13Features.shaderDemoteToHelperInvocation =
    physicalDeviceVulkan13Features.shaderDemoteToHelperInvocation;
  // Also enable useful Vulkan 1.2 features when supported by the device
  enabledPhysicalDeviceVulkan12Features.separateDepthStencilLayouts =
    physicalDeviceVulkan12Features.separateDepthStencilLayouts;
  enabledPhysicalDeviceVulkan12Features.uniformBufferStandardLayout =
    physicalDeviceVulkan12Features.uniformBufferStandardLayout;
  enabledPhysicalDeviceVulkan12Features.scalarBlockLayout = physicalDeviceVulkan12Features.scalarBlockLayout;
  enabledPhysicalDeviceVulkan12Features.drawIndirectCount = physicalDeviceVulkan12Features.drawIndirectCount;

  // Add platform-specific required extensions
#ifdef __APPLE__
  auto deviceExtensionProperties = m_physicalDevices[m_selectedDeviceIndex].enumerateDeviceExtensionProperties();
  addRequiredExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, enabledExtensions, deviceExtensionProperties, true);
#else
  auto deviceExtensionProperties = m_physicalDevices[m_selectedDeviceIndex].enumerateDeviceExtensionProperties();
#endif

  // Optional: enable calibrated timestamps device extension when available
  // Optional: memory budgeting (VK_EXT_memory_budget). Used for cache residency decisions.
  if (isExtensionAvailable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, deviceExtensionProperties)) {
    enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  }
  if (isExtensionAvailable(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, deviceExtensionProperties)) {
    enabledExtensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
  }

  // Optional: allow mixing inline draws and secondary command buffers inside a
  // vkCmdBeginRendering instance (needed for cached per-draw secondaries).
  const bool hasMaintenance7 = isExtensionAvailable(VK_KHR_MAINTENANCE_7_EXTENSION_NAME, deviceExtensionProperties);
  const bool hasNestedCmdBuf =
    isExtensionAvailable(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME, deviceExtensionProperties);
  if (hasMaintenance7) {
    enabledExtensions.push_back(VK_KHR_MAINTENANCE_7_EXTENSION_NAME);
  }
  if (hasNestedCmdBuf) {
    enabledExtensions.push_back(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME);
  }
  enabledPhysicalDeviceMaintenance7Features.maintenance7 =
    hasMaintenance7 ? physicalDeviceMaintenance7Features.maintenance7 : VK_FALSE;
  enabledPhysicalDeviceNestedCommandBufferFeatures.nestedCommandBuffer =
    hasNestedCmdBuf ? physicalDeviceNestedCommandBufferFeatures.nestedCommandBuffer : VK_FALSE;
  m_supportsInlineAndSecondaryDynamicRendering =
    (enabledPhysicalDeviceMaintenance7Features.maintenance7 == VK_TRUE) ||
    (enabledPhysicalDeviceNestedCommandBufferFeatures.nestedCommandBuffer == VK_TRUE);
  if (VLOG_IS_ON(1)) {
    VLOG(1) << fmt::format("VK nested command buffers: ext={} feat={} | maintenance7: ext={} feat={}",
                           hasNestedCmdBuf,
                           (enabledPhysicalDeviceNestedCommandBufferFeatures.nestedCommandBuffer == VK_TRUE),
                           hasMaintenance7,
                           (enabledPhysicalDeviceMaintenance7Features.maintenance7 == VK_TRUE));
  }

  // Create the logical device
  vk::DeviceCreateInfo deviceCreateInfo{
    .pNext = &enabledPhysicalDeviceFeatures2,
    .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
    .pQueueCreateInfos = queueCreateInfos.data(),
    .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
    .ppEnabledExtensionNames = enabledExtensions.data(),
  };

  m_device.emplace(m_physicalDevices[m_selectedDeviceIndex], deviceCreateInfo);
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
