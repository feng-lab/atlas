#include "zvulkancontext.h"
#include "zvulkanbindings.h"
#include "zvulkandevice.h"
#include "zvulkanmemoryutils.h"
#include "zvulkanuniforms.h"
#include "zexception.h"
#include "zlog.h"

#include <set>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include "zcommandlineflags.h"

ABSL_FLAG(bool, atlas_debug_vulkan, false, "Whether to enable Vulkan validation and debug utils");
ABSL_FLAG(int32_t, atlas_vk_device_index, -1, "Preferred Vulkan physical device index (sorted); -1 for auto");
ABSL_FLAG(int32_t, atlas_vk_frames_in_flight, 2, "Max Vulkan frames in flight (debug: set to 1 to serialize submits)");

ABSL_DECLARE_FLAG(int32_t, atlas_vk_bindless_texture2d_capacity);
ABSL_DECLARE_FLAG(int32_t, atlas_vk_bindless_texture2darray_capacity);
ABSL_DECLARE_FLAG(int32_t, atlas_vk_bindless_texture3d_capacity);
ABSL_DECLARE_FLAG(int32_t, atlas_vk_bindless_utexture2d_capacity);
ABSL_DECLARE_FLAG(int32_t, atlas_vk_bindless_utexture3d_capacity);

namespace nim {

namespace {

uint32_t configuredFrameSlotCount()
{
  return static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_frames_in_flight)));
}

} // namespace

std::string uuidToString(const std::array<uint8_t, VK_UUID_SIZE>& uuid)
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
  auto logLevel = absl::LogSeverity::kInfo;
  if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
    logLevel = absl::LogSeverity::kWarning;
  } else if (messageSeverity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
    logLevel = absl::LogSeverity::kError;
  }

  std::string message = pCallbackData->pMessage;
  if (logLevel == absl::LogSeverity::kWarning && message.find("VK_LOADER_DRIVERS_SELECT") != std::string::npos) {
    logLevel = absl::LogSeverity::kInfo;
  }

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

  LOG(LEVEL(logLevel)) << message;

  return VK_FALSE;
}

// ZVulkanContext implementation
ZVulkanContext::ZVulkanContext()
  : m_frameSlotCount(configuredFrameSlotCount())
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
  CHECK(m_liveDeviceWrapper == nullptr)
    << "Destroying Vulkan context before its device wrapper and device-owned resources";
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
    const uint64_t deviceLocalMemoryBytes = vulkanDeviceLocalMemoryBytes(pd.getMemoryProperties());
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
    fmt::format_to(std::back_inserter(summary),
                   "     Device Type:              {}\n",
                   enumOrUnderlying(props.deviceType, 16));
    fmt::format_to(std::back_inserter(summary),
                   "     Device-Local Memory:      {} MB\n",
                   deviceLocalMemoryBytes / (1024 * 1024));

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
  const bool debugVulkan = absl::GetFlag(FLAGS_atlas_debug_vulkan);

  if (debugVulkan) {
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

  if (debugVulkan) {
    instanceCreateInfo.setPNext(&debugUtilsMessengerCreateInfo);
  }

  m_instance.emplace(*m_context, instanceCreateInfo);
  LOG(INFO) << "Vulkan instance created successfully";
}

void ZVulkanContext::setupDebugMessenger()
{
  if (!absl::GetFlag(FLAGS_atlas_debug_vulkan) || !m_instance) {
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
using BindlessCapacities = ZVulkanDeviceSupport::BindlessSampledImageCapacities;
using DescriptorLimits = ZVulkanDeviceSupport::DescriptorLimits;
using ShaderResourcePolicy = ZVulkanDeviceSupport::ShaderResourcePolicy;

constexpr uint32_t kRequiredColorAttachmentCount = ShaderResourcePolicy::kMaximumColorOutputs;

BindlessCapacities requestedBindlessCapacities()
{
  BindlessCapacities requested{};
  requested.texture2D =
    static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_bindless_texture2d_capacity)));
  requested.texture2DArray =
    static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_bindless_texture2darray_capacity)));
  requested.texture3D =
    static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_bindless_texture3d_capacity)));
  requested.uTexture2D =
    static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_bindless_utexture2d_capacity)));
  requested.uTexture3D =
    static_cast<uint32_t>(std::max<int32_t>(1, absl::GetFlag(FLAGS_atlas_vk_bindless_utexture3d_capacity)));
  return requested;
}

DescriptorLimits descriptorLimitsFor(const vk::PhysicalDeviceProperties& properties,
                                     const vk::PhysicalDeviceDescriptorIndexingProperties& indexingProperties,
                                     bool updateAfterBind)
{
  if (updateAfterBind) {
    return {.perStageSamplers = indexingProperties.maxPerStageDescriptorUpdateAfterBindSamplers,
            .perSetSamplers = indexingProperties.maxDescriptorSetUpdateAfterBindSamplers,
            .perStageSampledImages = indexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages,
            .perSetSampledImages = indexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
            .perStageResources = indexingProperties.maxPerStageUpdateAfterBindResources};
  }
  return {.perStageSamplers = properties.limits.maxPerStageDescriptorSamplers,
          .perSetSamplers = properties.limits.maxDescriptorSetSamplers,
          .perStageSampledImages = properties.limits.maxPerStageDescriptorSampledImages,
          .perSetSampledImages = properties.limits.maxDescriptorSetSampledImages,
          .perStageResources = properties.limits.maxPerStageResources};
}

struct BindlessCapacityEvaluation
{
  BindlessCapacities effective{};
  bool clamped = false;
  std::string clampReason;
  std::string error;
};

BindlessCapacityEvaluation evaluateBindlessCapacities(const BindlessCapacities& requested,
                                                      const DescriptorLimits& limits)
{
  BindlessCapacityEvaluation result{.effective = requested};

  constexpr uint32_t kSamplerCountTotal = 3u;
  constexpr uint32_t kSamplerCountFragment = 3u;
  constexpr uint32_t kMinimumFragmentSampledImages = 4u;
  constexpr uint32_t kMinimumComputeSampledImages = 1u;
  constexpr uint32_t kMinimumTotalSampledImages = kMinimumFragmentSampledImages + kMinimumComputeSampledImages;
  const auto fragmentAggregateBudget = ShaderResourcePolicy::fragmentBindlessBudget(limits.perStageResources);
  const auto computeAggregateBudget = ShaderResourcePolicy::computeBindlessBudget(limits.perStageResources);

  if (limits.perStageSamplers < kSamplerCountFragment || limits.perSetSamplers < kSamplerCountTotal ||
      limits.perStageSampledImages < kMinimumFragmentSampledImages ||
      limits.perStageSampledImages < kMinimumComputeSampledImages ||
      limits.perSetSampledImages < kMinimumTotalSampledImages || !fragmentAggregateBudget.has_value() ||
      *fragmentAggregateBudget < kMinimumFragmentSampledImages || !computeAggregateBudget.has_value() ||
      *computeAggregateBudget < kMinimumComputeSampledImages) {
    result.error = fmt::format(
      "bindless descriptor limits are insufficient (need samplers: per-stage>={} per-set>={}; sampled images: "
      "per-stage>={} per-set>={}; aggregate resources: fragment>={} compute>={} | reported samplers={}/{} "
      "sampled-images={}/{} aggregate={})",
      kSamplerCountFragment,
      kSamplerCountTotal,
      kMinimumFragmentSampledImages,
      kMinimumTotalSampledImages,
      ShaderResourcePolicy::kGraphicsFragmentFixedResources + kMinimumFragmentSampledImages,
      ShaderResourcePolicy::kComputeFixedResources + kMinimumComputeSampledImages,
      limits.perStageSamplers,
      limits.perSetSamplers,
      limits.perStageSampledImages,
      limits.perSetSampledImages,
      limits.perStageResources);
    return result;
  }

  auto clampByPriority = [&result](std::initializer_list<uint32_t*> ordered, uint32_t limit, std::string_view reason) {
    uint64_t sum = 0u;
    for (const auto* value : ordered) {
      sum += *value;
    }
    if (sum <= limit) {
      return;
    }
    uint64_t toReduce = sum - limit;
    for (auto* value : ordered) {
      if (toReduce == 0u) {
        break;
      }
      const uint64_t reducible = *value - 1u;
      const uint64_t decrement = std::min(reducible, toReduce);
      *value -= static_cast<uint32_t>(decrement);
      toReduce -= decrement;
    }
    CHECK(toReduce == 0u) << "Validated bindless descriptor limits failed to preserve minimum capacities";
    result.clamped = true;
    if (!result.clampReason.empty()) {
      result.clampReason.append(", ");
    }
    result.clampReason.append(reason);
  };

  clampByPriority({&result.effective.uTexture2D},
                  std::min(limits.perStageSampledImages, *computeAggregateBudget),
                  "compute-stage bindless sampled-image capacity");
  clampByPriority({&result.effective.texture2D,
                   &result.effective.texture2DArray,
                   &result.effective.texture3D,
                   &result.effective.uTexture3D},
                  std::min(limits.perStageSampledImages, *fragmentAggregateBudget),
                  "fragment-stage bindless sampled-image capacity");
  clampByPriority({&result.effective.texture2D,
                   &result.effective.texture2DArray,
                   &result.effective.texture3D,
                   &result.effective.uTexture3D,
                   &result.effective.uTexture2D},
                  limits.perSetSampledImages,
                  "descriptor-set bindless sampled-image capacity");
  return result;
}

struct ImageFormatRequirement
{
  vk::Format format = vk::Format::eUndefined;
  vk::ImageType type = vk::ImageType::e2D;
  vk::ImageUsageFlags usage{};
  bool requireLinearFiltering = false;
  std::string_view label;
};

constexpr auto kColorScratchUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
constexpr auto kDepthScratchUsage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled |
                                    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
constexpr auto kSampledUploadUsage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;

const std::array<ImageFormatRequirement, 14> kImageFormatRequirements{
  ImageFormatRequirement{vk::Format::eR8G8B8A8Unorm,      vk::ImageType::e2D, kColorScratchUsage,  true,  "RGBA8 scratch"            },
  ImageFormatRequirement{vk::Format::eR32G32B32A32Uint,
                         vk::ImageType::e2D,
                         kColorScratchUsage | vk::ImageUsageFlagBits::eStorage,
                         false,                                                                           "RGBA32UI block-ID scratch"},
  ImageFormatRequirement{vk::Format::eR32G32B32A32Sfloat,
                         vk::ImageType::e2D,
                         kColorScratchUsage,                                                       false,
                         "RGBA32F scratch"                                                                                           },
  ImageFormatRequirement{vk::Format::eR16G16B16A16Unorm,
                         vk::ImageType::e2D,
                         kColorScratchUsage,                                                       true,
                         "RGBA16 scratch"                                                                                            },
  ImageFormatRequirement{vk::Format::eR16G16B16A16Sfloat,
                         vk::ImageType::e2D,
                         kColorScratchUsage,                                                       true,
                         "RGBA16F scratch"                                                                                           },
  ImageFormatRequirement{vk::Format::eR32G32Sfloat,       vk::ImageType::e2D, kColorScratchUsage,  false, "RG32F scratch"            },
  ImageFormatRequirement{vk::Format::eR32Sfloat,          vk::ImageType::e2D, kColorScratchUsage,  false, "R32F scratch"             },
  ImageFormatRequirement{vk::Format::eR16Sfloat,          vk::ImageType::e2D, kColorScratchUsage,  true,  "R16F scratch"             },
  ImageFormatRequirement{vk::Format::eD32Sfloat,          vk::ImageType::e2D, kDepthScratchUsage,  false, "D32F scratch"             },
  ImageFormatRequirement{vk::Format::eR8Unorm,            vk::ImageType::e2D, kSampledUploadUsage, true,  "R8 2D image"              },
  ImageFormatRequirement{vk::Format::eR8Unorm,
                         vk::ImageType::e3D,
                         kSampledUploadUsage | vk::ImageUsageFlagBits::eTransferSrc,
                         true,                                                                            "R8 volume/page cache"     },
  ImageFormatRequirement{vk::Format::eR8G8B8A8Unorm,
                         vk::ImageType::e2D,
                         kSampledUploadUsage,                                                      true,
                         "RGBA8 LUT/placeholder"                                                                                     },
  ImageFormatRequirement{vk::Format::eB8G8R8A8Unorm,      vk::ImageType::e2D, kSampledUploadUsage, true,  "BGRA8 font atlas"         },
  ImageFormatRequirement{vk::Format::eR32G32B32A32Uint,
                         vk::ImageType::e3D,
                         kSampledUploadUsage,                                                      false,
                         "RGBA32UI paged metadata"                                                                                   },
};
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
    ZVulkanDeviceSupport support{};
    vk::PhysicalDeviceProperties props{};
    ZVulkanDeviceSupport::PhysicalDevicePreference preference{};
  };

  std::vector<DeviceInfo> infos;
  infos.reserve(enumerated.size());

  // Collect information and suitability for each device
  for (auto& pd : enumerated) {
    DeviceInfo info;
    info.device = std::move(pd);
    const auto properties = info.device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
    info.props = properties.get<vk::PhysicalDeviceProperties2>().properties;
    info.preference.deviceType = info.props.deviceType;
    info.preference.deviceLocalMemoryBytes = vulkanDeviceLocalMemoryBytes(info.device.getMemoryProperties());
    info.preference.apiVersion = info.props.apiVersion;
    const auto& deviceUuid = properties.get<vk::PhysicalDeviceIDProperties>().deviceUUID;
    std::copy(deviceUuid.begin(), deviceUuid.end(), info.preference.deviceUuid.begin());
    info.support = evaluateDeviceSupport(info.device);

    infos.emplace_back(std::move(info));
  }

  // Sort by power, then use the immutable device UUID to keep public indices
  // stable when separate worker processes enumerate equally ranked adapters.
  std::sort(infos.begin(), infos.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
    return ZVulkanDeviceSupport::PhysicalDevicePreference::isPreferredBefore(a.preference, b.preference);
  });

  // Move sorted devices into member vector
  m_physicalDevices.clear();
  m_deviceSupports.clear();
  m_physicalDevices.reserve(infos.size());
  m_deviceSupports.reserve(infos.size());
  for (auto& di : infos) {
    m_physicalDevices.emplace_back(std::move(di.device));
    m_deviceSupports.emplace_back(std::move(di.support));
  }

  // The explicit index is a preference. If it cannot be honored, select the
  // first fully compatible device and make the fallback visible in the log.
  const int32_t preferredDeviceIndex = absl::GetFlag(FLAGS_atlas_vk_device_index);
  const std::optional<size_t> requestedIndex =
    preferredDeviceIndex >= 0 ? std::optional<size_t>(static_cast<size_t>(preferredDeviceIndex)) : std::nullopt;
  if (preferredDeviceIndex < -1) {
    LOG(WARNING) << fmt::format("Preferred Vulkan device index {} is invalid; using automatic device selection",
                                preferredDeviceIndex);
  }
  auto selection = ZVulkanDeviceSupport::select(m_deviceSupports, requestedIndex);
  if (!selection.warning.empty()) {
    LOG(WARNING) << selection.warning;
  }
  if (!selection.index.has_value()) {
    throw ZException(selection.error);
  }
  m_selectedDeviceIndex = *selection.index;
  const auto& selectedSupport = selectedDeviceSupport();
  CHECK(selectedSupport.graphicsFamily.has_value());
  m_queueFamilyIndices.graphicsFamily = selectedSupport.graphicsFamily;
  // Atlas is offscreen-only today; retain the existing queue alias until the
  // remaining pseudo-presentation API is removed separately.
  m_queueFamilyIndices.presentFamily = selectedSupport.graphicsFamily;

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
    LOG(INFO) << fmt::format("      Device Type:          {}", enumOrUnderlying(p.deviceType, 16));
    LOG(INFO) << fmt::format("      Device UUID:          {}", uuidToString(infos[i].preference.deviceUuid));
    LOG(INFO) << fmt::format("      Device-Local Memory:  {} MB",
                             infos[i].preference.deviceLocalMemoryBytes / (1024 * 1024));
    if (!m_deviceSupports[i].compatible()) {
      LOG(INFO) << fmt::format("      Atlas compatibility:  rejected ({})", m_deviceSupports[i].rejectionSummary());
    } else {
      LOG(INFO) << "      Atlas compatibility:  compatible";
    }
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

ZVulkanDeviceSupport ZVulkanContext::evaluateDeviceSupport(vk::raii::PhysicalDevice& physicalDevice) const
{
  ZVulkanDeviceSupport result;
  const auto properties = physicalDevice.getProperties();
  const auto extensions = physicalDevice.enumerateDeviceExtensionProperties();
  const auto queueFamilies = findQueueFamilies(physicalDevice);
  result.graphicsFamily = queueFamilies.graphicsFamily;
  result.maxComputeWorkGroupCountX = properties.limits.maxComputeWorkGroupCount[0];

  auto reject = [&result](ZVulkanDeviceSupport::RejectionCode code, std::string detail) {
    result.rejections.push_back({.code = code, .detail = std::move(detail)});
  };

  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    reject(ZVulkanDeviceSupport::RejectionCode::ApiVersion,
           fmt::format("Vulkan 1.3 is required, device exposes {}", versionToString(properties.apiVersion)));
  }
  if (!queueFamilies.graphicsFamily.has_value()) {
    reject(ZVulkanDeviceSupport::RejectionCode::GraphicsQueue, "no graphics-capable queue family");
  }

#ifdef __APPLE__
  const bool portabilitySubsetAvailable = isExtensionAvailable(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, extensions);
  if (!portabilitySubsetAvailable) {
    reject(ZVulkanDeviceSupport::RejectionCode::RequiredExtension,
           fmt::format("missing required extension {}", VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME));
  }
#endif

  const auto features2 = physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2,
                                                     vk::PhysicalDeviceVulkan12Features,
                                                     vk::PhysicalDeviceVulkan13Features>();
  const auto& features = features2.get<vk::PhysicalDeviceFeatures2>().features;
  const auto& features12 = features2.get<vk::PhysicalDeviceVulkan12Features>();
  const auto& features13 = features2.get<vk::PhysicalDeviceVulkan13Features>();

#ifdef __APPLE__
  if (portabilitySubsetAvailable) {
    const auto portabilityFeatures2 =
      physicalDevice.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
    result.portabilityTriangleFans =
      portabilityFeatures2.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>().triangleFans;
    if (!result.portabilityTriangleFans) {
      reject(ZVulkanDeviceSupport::RejectionCode::PortabilityTriangleFans,
             "VK_KHR_portability_subset triangleFans is required by mesh triangle-fan rendering");
    }
  }
#endif

  if (!features.shaderClipDistance) {
    reject(ZVulkanDeviceSupport::RejectionCode::ShaderClipDistance,
           "shaderClipDistance is required for XYZ cut clip planes");
  }
  if (properties.limits.maxClipDistances < kVulkanMaxClipDistances ||
      properties.limits.maxCombinedClipAndCullDistances < kVulkanMaxClipDistances) {
    reject(ZVulkanDeviceSupport::RejectionCode::ClipDistanceLimit,
           fmt::format("Atlas requires {} clip distances, device reports maxClipDistances={} and "
                       "maxCombinedClipAndCullDistances={}",
                       kVulkanMaxClipDistances,
                       properties.limits.maxClipDistances,
                       properties.limits.maxCombinedClipAndCullDistances));
  }
  if (!features.independentBlend) {
    reject(ZVulkanDeviceSupport::RejectionCode::IndependentBlend,
           "independentBlend is required by multi-attachment transparency passes");
  }
  if (!features.fillModeNonSolid) {
    reject(ZVulkanDeviceSupport::RejectionCode::FillModeNonSolid,
           "fillModeNonSolid is required by mesh wireframe rendering");
  }
  if (!features.fragmentStoresAndAtomics) {
    reject(ZVulkanDeviceSupport::RejectionCode::FragmentStoresAndAtomics,
           "fragmentStoresAndAtomics is required by Vulkan OIT passes");
  }
  if (properties.limits.maxColorAttachments < kRequiredColorAttachmentCount) {
    reject(ZVulkanDeviceSupport::RejectionCode::ColorAttachmentLimit,
           fmt::format("Atlas transparency requires {} color attachments, device reports {}",
                       kRequiredColorAttachmentCount,
                       properties.limits.maxColorAttachments));
  }
  if (properties.limits.maxFragmentOutputAttachments < ShaderResourcePolicy::kMaximumColorOutputs) {
    reject(ZVulkanDeviceSupport::RejectionCode::FragmentOutputAttachmentLimit,
           fmt::format("Block-ID rendering requires {} fragment output attachments, device reports {}",
                       ShaderResourcePolicy::kMaximumColorOutputs,
                       properties.limits.maxFragmentOutputAttachments));
  }
  if (properties.limits.maxFragmentCombinedOutputResources <
      ShaderResourcePolicy::kRequiredFragmentCombinedOutputResources) {
    reject(ZVulkanDeviceSupport::RejectionCode::FragmentCombinedOutputLimit,
           fmt::format("Atlas fragment shaders require {} combined output resources (eight-output Block-ID rendering; "
                       "PPLL resolve requires {}), device reports {}",
                       ShaderResourcePolicy::kRequiredFragmentCombinedOutputResources,
                       ShaderResourcePolicy::kPPLLFragmentCombinedOutputResources,
                       properties.limits.maxFragmentCombinedOutputResources));
  }
  if (properties.limits.maxComputeWorkGroupInvocations < ShaderResourcePolicy::kRequiredComputeWorkGroupInvocations ||
      properties.limits.maxComputeWorkGroupSize[0] < ShaderResourcePolicy::kRequiredComputeWorkGroupSizeX ||
      properties.limits.maxComputeWorkGroupSize[1] < ShaderResourcePolicy::kRequiredComputeWorkGroupSizeY ||
      properties.limits.maxComputeWorkGroupCount[0] < ShaderResourcePolicy::kMinimumComputeWorkGroupCountX) {
    reject(ZVulkanDeviceSupport::RejectionCode::ComputeWorkGroupLimits,
           fmt::format("Atlas compute shaders require max invocations>={}, workgroup size x>={}, y>={}; device reports "
                       "invocations={} size={}x{}x{} count-x={}",
                       ShaderResourcePolicy::kRequiredComputeWorkGroupInvocations,
                       ShaderResourcePolicy::kRequiredComputeWorkGroupSizeX,
                       ShaderResourcePolicy::kRequiredComputeWorkGroupSizeY,
                       properties.limits.maxComputeWorkGroupInvocations,
                       properties.limits.maxComputeWorkGroupSize[0],
                       properties.limits.maxComputeWorkGroupSize[1],
                       properties.limits.maxComputeWorkGroupSize[2],
                       properties.limits.maxComputeWorkGroupCount[0]));
  }
  if (!features13.dynamicRendering) {
    reject(ZVulkanDeviceSupport::RejectionCode::DynamicRendering, "dynamicRendering is required by the Vulkan backend");
  }
  if (!features13.synchronization2) {
    reject(ZVulkanDeviceSupport::RejectionCode::Synchronization2, "synchronization2 is required by the Vulkan backend");
  }
  if (properties.limits.maxPerStageDescriptorStorageBuffers < vkbind::kOITStorageBufferBindingCount ||
      properties.limits.maxDescriptorSetStorageBuffers < vkbind::kOITStorageBufferBindingCount) {
    reject(ZVulkanDeviceSupport::RejectionCode::DescriptorCapacity,
           fmt::format("Atlas' shared OIT layout requires {} fragment-stage storage buffers; device reports "
                       "maxPerStageDescriptorStorageBuffers={} and maxDescriptorSetStorageBuffers={}",
                       vkbind::kOITStorageBufferBindingCount,
                       properties.limits.maxPerStageDescriptorStorageBuffers,
                       properties.limits.maxDescriptorSetStorageBuffers));
  }

  const bool descriptorFeaturesPresent = features12.descriptorIndexing && features12.runtimeDescriptorArray &&
                                         features12.shaderSampledImageArrayNonUniformIndexing &&
                                         features12.descriptorBindingPartiallyBound;
  if (!descriptorFeaturesPresent) {
    reject(ZVulkanDeviceSupport::RejectionCode::DescriptorIndexing,
           fmt::format("required descriptor indexing features are missing "
                       "(descriptorIndexing={} runtimeDescriptorArray={} "
                       "shaderSampledImageArrayNonUniformIndexing={} descriptorBindingPartiallyBound={})",
                       static_cast<bool>(features12.descriptorIndexing),
                       static_cast<bool>(features12.runtimeDescriptorArray),
                       static_cast<bool>(features12.shaderSampledImageArrayNonUniformIndexing),
                       static_cast<bool>(features12.descriptorBindingPartiallyBound)));
  }

  result.requestedBindlessCapacities = requestedBindlessCapacities();
  const auto properties2 =
    physicalDevice.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorIndexingProperties>();
  const auto& indexingProperties = properties2.get<vk::PhysicalDeviceDescriptorIndexingProperties>();
  result.maxUpdateAfterBindDescriptorsInAllPools = indexingProperties.maxUpdateAfterBindDescriptorsInAllPools;

  // Update-after-bind is optional. Prefer its usually larger limit class only
  // when both the feature and the reported limits satisfy Atlas' contract;
  // otherwise fall back to the legacy descriptor limits.
  BindlessCapacityEvaluation bindlessEvaluation;
  if (features12.descriptorBindingSampledImageUpdateAfterBind) {
    const uint32_t frameSlots = m_frameSlotCount;
    auto updateAfterBindLimits = descriptorLimitsFor(properties, indexingProperties, true);
    const auto globalBindlessBudget = ZVulkanDeviceSupport::DescriptorPoolPolicy::maxBindlessSampledImagesPerFrameSlot(
      indexingProperties.maxUpdateAfterBindDescriptorsInAllPools,
      frameSlots);
    if (globalBindlessBudget.has_value()) {
      updateAfterBindLimits.perSetSampledImages =
        std::min(updateAfterBindLimits.perSetSampledImages, *globalBindlessBudget);
      bindlessEvaluation = evaluateBindlessCapacities(result.requestedBindlessCapacities, updateAfterBindLimits);
      if (bindlessEvaluation.error.empty()) {
        const auto requiredDescriptors =
          ZVulkanDeviceSupport::DescriptorPoolPolicy::requiredUpdateAfterBindDescriptors(bindlessEvaluation.effective,
                                                                                         frameSlots);
        CHECK(requiredDescriptors.has_value()) << "Validated update-after-bind pool budget overflowed";
        CHECK_LE(*requiredDescriptors,
                 static_cast<uint64_t>(indexingProperties.maxUpdateAfterBindDescriptorsInAllPools));
        result.descriptorIndexingSampledImageUpdateAfterBind = true;
        result.requiredUpdateAfterBindDescriptors = *requiredDescriptors;
        result.descriptorLimits = updateAfterBindLimits;
      } else {
        result.updateAfterBindFallbackReason = std::move(bindlessEvaluation.error);
      }
    } else {
      result.updateAfterBindFallbackReason =
        fmt::format("global update-after-bind descriptor budget {} cannot reserve {} immutable samplers for each of "
                    "{} frame slots",
                    indexingProperties.maxUpdateAfterBindDescriptorsInAllPools,
                    ZVulkanDeviceSupport::DescriptorPoolPolicy::kBindlessSamplerDescriptors,
                    frameSlots);
    }
  }
  if (!result.descriptorIndexingSampledImageUpdateAfterBind) {
    result.descriptorLimits = descriptorLimitsFor(properties, indexingProperties, false);
    bindlessEvaluation = evaluateBindlessCapacities(result.requestedBindlessCapacities, result.descriptorLimits);
  }
  if (!bindlessEvaluation.error.empty()) {
    if (!result.updateAfterBindFallbackReason.empty()) {
      bindlessEvaluation.error.append("; update-after-bind path unavailable: ");
      bindlessEvaluation.error.append(result.updateAfterBindFallbackReason);
    }
    reject(ZVulkanDeviceSupport::RejectionCode::DescriptorCapacity, std::move(bindlessEvaluation.error));
  } else {
    result.effectiveBindlessCapacities = bindlessEvaluation.effective;
    result.bindlessCapacitiesClamped = bindlessEvaluation.clamped;
    result.bindlessClampReason = std::move(bindlessEvaluation.clampReason);
  }

  for (const auto& requirement : kImageFormatRequirements) {
    try {
      (void)physicalDevice.getImageFormatProperties(requirement.format,
                                                    requirement.type,
                                                    vk::ImageTiling::eOptimal,
                                                    requirement.usage);
    }
    catch (const vk::SystemError& error) {
      reject(ZVulkanDeviceSupport::RejectionCode::ImageFormatContract,
             fmt::format("{} format {} does not support image type {} and usage 0x{:x}: {}",
                         requirement.label,
                         enumOrUnderlying(requirement.format, 16),
                         enumOrUnderlying(requirement.type, 16),
                         static_cast<VkImageUsageFlags>(requirement.usage),
                         error.what()));
      continue;
    }

    if (requirement.requireLinearFiltering) {
      const auto formatProperties = physicalDevice.getFormatProperties(requirement.format);
      if (!(formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)) {
        reject(ZVulkanDeviceSupport::RejectionCode::ImageFormatContract,
               fmt::format("{} format {} does not support linear filtering with optimal tiling",
                           requirement.label,
                           enumOrUnderlying(requirement.format, 16)));
      }
    }
  }

  result.memoryBudget = isExtensionAvailable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, extensions);
  result.calibratedTimestamps = isExtensionAvailable(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, extensions);
  result.maintenance7 = isExtensionAvailable(VK_KHR_MAINTENANCE_7_EXTENSION_NAME, extensions);
  result.nestedCommandBuffer = isExtensionAvailable(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME, extensions);
  return result;
}

void ZVulkanContext::createLogicalDevice()
{
  if (m_physicalDevices.empty()) {
    throw ZException("Attempted to create logical device without a physical device");
  }
  const auto& support = selectedDeviceSupport();
  if (!support.compatible()) {
    throw ZException(fmt::format("Attempted to create a logical device from an incompatible physical device: {}",
                                 support.rejectionSummary()));
  }
  CHECK(support.graphicsFamily.has_value());
  CHECK(m_queueFamilyIndices.graphicsFamily == support.graphicsFamily);

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
#ifdef __APPLE__
  auto features2 = m_physicalDevices[m_selectedDeviceIndex]
                     .getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDeviceVulkan13Features,
                                   vk::PhysicalDeviceMaintenance7FeaturesKHR,
                                   vk::PhysicalDeviceNestedCommandBufferFeaturesEXT,
                                   vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
#else
  auto features2 = m_physicalDevices[m_selectedDeviceIndex]
                     .getFeatures2<vk::PhysicalDeviceFeatures2,
                                   vk::PhysicalDeviceVulkan12Features,
                                   vk::PhysicalDeviceVulkan13Features,
                                   vk::PhysicalDeviceMaintenance7FeaturesKHR,
                                   vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();
#endif
  auto& physicalDeviceFeatures = features2.get<vk::PhysicalDeviceFeatures2>().features;
  auto& physicalDeviceVulkan12Features = features2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& physicalDeviceVulkan13Features = features2.get<vk::PhysicalDeviceVulkan13Features>();
  auto& physicalDeviceMaintenance7Features = features2.get<vk::PhysicalDeviceMaintenance7FeaturesKHR>();
  auto& physicalDeviceNestedCommandBufferFeatures = features2.get<vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();
#ifdef __APPLE__
  auto& physicalDevicePortabilityFeatures = features2.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
#endif

  // Setup enabled features
#ifdef __APPLE__
  vk::StructureChain<vk::PhysicalDeviceFeatures2,
                     vk::PhysicalDeviceVulkan12Features,
                     vk::PhysicalDeviceVulkan13Features,
                     vk::PhysicalDeviceMaintenance7FeaturesKHR,
                     vk::PhysicalDeviceNestedCommandBufferFeaturesEXT,
                     vk::PhysicalDevicePortabilitySubsetFeaturesKHR>
    enabledFeatures2;
#else
  vk::StructureChain<vk::PhysicalDeviceFeatures2,
                     vk::PhysicalDeviceVulkan12Features,
                     vk::PhysicalDeviceVulkan13Features,
                     vk::PhysicalDeviceMaintenance7FeaturesKHR,
                     vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>
    enabledFeatures2;
#endif
  auto& enabledPhysicalDeviceFeatures2 = enabledFeatures2.get<vk::PhysicalDeviceFeatures2>();
  auto& enabledPhysicalDeviceVulkan12Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan12Features>();
  auto& enabledPhysicalDeviceVulkan13Features = enabledFeatures2.get<vk::PhysicalDeviceVulkan13Features>();
  auto& enabledPhysicalDeviceMaintenance7Features = enabledFeatures2.get<vk::PhysicalDeviceMaintenance7FeaturesKHR>();
  auto& enabledPhysicalDeviceNestedCommandBufferFeatures =
    enabledFeatures2.get<vk::PhysicalDeviceNestedCommandBufferFeaturesEXT>();
#ifdef __APPLE__
  auto& enabledPhysicalDevicePortabilityFeatures =
    enabledFeatures2.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
#endif

  // Clip planes are required for parity with the OpenGL backend's local/global
  // XYZ cuts. These use gl_ClipDistance in vertex shaders, so we must enable
  // the Vulkan core shaderClipDistance feature and validate the device limit.
  const auto properties = m_physicalDevices[m_selectedDeviceIndex].getProperties();
  CHECK(physicalDeviceFeatures.shaderClipDistance);
  CHECK(properties.limits.maxClipDistances >= kVulkanMaxClipDistances);
  CHECK(properties.limits.maxCombinedClipAndCullDistances >= kVulkanMaxClipDistances);
  CHECK(properties.limits.maxPerStageDescriptorStorageBuffers >= vkbind::kOITStorageBufferBindingCount);
  CHECK(properties.limits.maxDescriptorSetStorageBuffers >= vkbind::kOITStorageBufferBindingCount);
  CHECK(properties.limits.maxFragmentOutputAttachments >= ShaderResourcePolicy::kMaximumColorOutputs);
  CHECK(properties.limits.maxFragmentCombinedOutputResources >=
        ShaderResourcePolicy::kRequiredFragmentCombinedOutputResources);
  CHECK(properties.limits.maxComputeWorkGroupInvocations >= ShaderResourcePolicy::kRequiredComputeWorkGroupInvocations);
  CHECK(properties.limits.maxComputeWorkGroupSize[0] >= ShaderResourcePolicy::kRequiredComputeWorkGroupSizeX);
  CHECK(properties.limits.maxComputeWorkGroupSize[1] >= ShaderResourcePolicy::kRequiredComputeWorkGroupSizeY);
  CHECK_GT(support.maxComputeWorkGroupCountX, 0u);

  // Enable basic features
  enabledPhysicalDeviceFeatures2.features.samplerAnisotropy = physicalDeviceFeatures.samplerAnisotropy;
  CHECK(physicalDeviceFeatures.fillModeNonSolid);
  enabledPhysicalDeviceFeatures2.features.fillModeNonSolid = true;
  // Enable independentBlend if supported to allow per-attachment blend state
  CHECK(physicalDeviceFeatures.independentBlend);
  enabledPhysicalDeviceFeatures2.features.independentBlend = true;
  // Storage buffer/image writes in fragment shaders are required by Vulkan OIT.
  CHECK(physicalDeviceFeatures.fragmentStoresAndAtomics);
  enabledPhysicalDeviceFeatures2.features.fragmentStoresAndAtomics = true;
  enabledPhysicalDeviceFeatures2.features.shaderClipDistance = true;

#ifdef __APPLE__
  CHECK(support.portabilityTriangleFans);
  CHECK(physicalDevicePortabilityFeatures.triangleFans);
  enabledPhysicalDevicePortabilityFeatures.triangleFans = true;
#endif

  if (absl::GetFlag(FLAGS_atlas_debug_vulkan)) {
#ifdef __APPLE__
    // MoltenVK/Metal does not currently support Vulkan buffer robustness; requesting
    // robustBufferAccess triggers noisy warnings like:
    //   "VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support buffer robustness."
    //
    // Keep this disabled on macOS even in debug mode (Atlas should not rely on
    // robust buffer access semantics for correctness).
    enabledPhysicalDeviceFeatures2.features.robustBufferAccess = false;
#else
    enabledPhysicalDeviceFeatures2.features.robustBufferAccess = physicalDeviceFeatures.robustBufferAccess;
#endif
  }

  // Enable required extensions
  std::vector<const char*> enabledExtensions;

  // Vulkan 1.3 is required, but individual features are still optional and must
  // be queried/enabled explicitly.
  CHECK(physicalDeviceVulkan13Features.dynamicRendering);
  CHECK(physicalDeviceVulkan13Features.synchronization2);
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

  // ---------------------------------------------------------------------------
  // Descriptor indexing (bindless sampled images)
  // ---------------------------------------------------------------------------
  // Atlas' Vulkan backend relies on descriptor indexing + runtime descriptor
  // arrays to eliminate per-draw descriptor churn and to keep descriptor
  // updates outside command-buffer recording. We intentionally do NOT require
  // update-after-bind for correctness: the backend mutates per-frame-slot
  // descriptor tables only after the slot's previous submission fence is
  // observed complete. However, some drivers (notably MoltenVK) report very
  // small legacy per-stage sampler limits; enabling update-after-bind when
  // available allows large bindless arrays to be validated against the
  // descriptor indexing update-after-bind limits instead.
  const bool hasDescriptorIndexing = (physicalDeviceVulkan12Features.descriptorIndexing == VK_TRUE);
  const bool hasRuntimeDescriptorArray = (physicalDeviceVulkan12Features.runtimeDescriptorArray == VK_TRUE);
  const bool hasNonUniformSampledImages =
    (physicalDeviceVulkan12Features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE);
  const bool hasPartiallyBound = (physicalDeviceVulkan12Features.descriptorBindingPartiallyBound == VK_TRUE);
  CHECK(hasDescriptorIndexing);
  CHECK(hasRuntimeDescriptorArray);
  CHECK(hasNonUniformSampledImages);
  CHECK(hasPartiallyBound);
  enabledPhysicalDeviceVulkan12Features.descriptorIndexing = true;
  enabledPhysicalDeviceVulkan12Features.runtimeDescriptorArray = true;
  enabledPhysicalDeviceVulkan12Features.shaderSampledImageArrayNonUniformIndexing = true;
  enabledPhysicalDeviceVulkan12Features.descriptorBindingPartiallyBound = true;

  // Optional: enable update-after-bind for sampled images when available. Atlas
  // does not update bindless tables while they can be read by the GPU; this is
  // enabled to satisfy descriptor limit accounting on platforms with low
  // legacy sampler limits (e.g., MoltenVK).
  m_supportsDescriptorIndexingSampledImageUpdateAfterBind = support.descriptorIndexingSampledImageUpdateAfterBind;
  if (m_supportsDescriptorIndexingSampledImageUpdateAfterBind) {
    CHECK(physicalDeviceVulkan12Features.descriptorBindingSampledImageUpdateAfterBind);
    enabledPhysicalDeviceVulkan12Features.descriptorBindingSampledImageUpdateAfterBind = true;
  }

  // Optional: enable variable descriptor count support when available. Atlas
  // currently sizes bindless tables explicitly, but keeping this enabled when
  // supported is future-proof for layout evolution.
  enabledPhysicalDeviceVulkan12Features.descriptorBindingVariableDescriptorCount =
    physicalDeviceVulkan12Features.descriptorBindingVariableDescriptorCount;

  computeBindlessSampledImageCapacities();

  // Add platform-specific required extensions
#ifdef __APPLE__
  auto deviceExtensionProperties = m_physicalDevices[m_selectedDeviceIndex].enumerateDeviceExtensionProperties();
  addRequiredExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, enabledExtensions, deviceExtensionProperties);
#else
  auto deviceExtensionProperties = m_physicalDevices[m_selectedDeviceIndex].enumerateDeviceExtensionProperties();
#endif

  // Optional: memory budgeting (VK_EXT_memory_budget). Used for cache residency decisions.
  if (support.memoryBudget) {
    CHECK(isExtensionAvailable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, deviceExtensionProperties));
    enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  }
  m_supportsCalibratedTimestamps = support.calibratedTimestamps;
  if (m_supportsCalibratedTimestamps) {
    CHECK(isExtensionAvailable(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, deviceExtensionProperties));
    enabledExtensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
  }

  // Optional: allow mixing inline draws and secondary command buffers inside a
  // vkCmdBeginRendering instance (needed for cached per-draw secondaries).
  const bool hasMaintenance7 = support.maintenance7;
  const bool hasNestedCmdBuf = support.nestedCommandBuffer;
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

void ZVulkanContext::computeBindlessSampledImageCapacities()
{
  const auto& support = selectedDeviceSupport();
  CHECK(support.compatible());
  m_requestedBindlessSampledImageCapacities = support.requestedBindlessCapacities;
  m_effectiveBindlessSampledImageCapacities = support.effectiveBindlessCapacities;
  m_bindlessSampledImageCapacitiesClamped = support.bindlessCapacitiesClamped;

  const auto& requested = m_requestedBindlessSampledImageCapacities;
  const auto& effective = m_effectiveBindlessSampledImageCapacities;
  const auto& limits = support.descriptorLimits;

  const auto reqTotal = requested.totalSampledImages();
  const auto reqFrag = requested.fragmentVisibleSampledImages();
  const auto reqComp = requested.computeVisibleSampledImages();
  const auto effTotal = effective.totalSampledImages();
  const auto effFrag = effective.fragmentVisibleSampledImages();
  const auto effComp = effective.computeVisibleSampledImages();

  LOG(INFO) << fmt::format(
    "VK bindless sampled-image capacity policy: update_after_bind={} requested={{2d={} 2darray={} 3d={} u2d={} u3d={} "
    "total={} frag={} comp={}}} effective={{2d={} 2darray={} 3d={} u2d={} u3d={} total={} frag={} comp={}}} "
    "limits={{perStageSamplers={} perSetSamplers={} perStageSampledImages={} perSetSampledImages={} "
    "perStageResources={}}} "
    "uab_pool_descriptors={}/{}",
    m_supportsDescriptorIndexingSampledImageUpdateAfterBind,
    requested.texture2D,
    requested.texture2DArray,
    requested.texture3D,
    requested.uTexture2D,
    requested.uTexture3D,
    reqTotal,
    reqFrag,
    reqComp,
    effective.texture2D,
    effective.texture2DArray,
    effective.texture3D,
    effective.uTexture2D,
    effective.uTexture3D,
    effTotal,
    effFrag,
    effComp,
    limits.perStageSamplers,
    limits.perSetSamplers,
    limits.perStageSampledImages,
    limits.perSetSampledImages,
    limits.perStageResources,
    support.requiredUpdateAfterBindDescriptors,
    support.maxUpdateAfterBindDescriptorsInAllPools);

  if (!m_supportsDescriptorIndexingSampledImageUpdateAfterBind && !support.updateAfterBindFallbackReason.empty()) {
    VLOG(1) << "VK update-after-bind descriptor path not selected: " << support.updateAfterBindFallbackReason;
  }

  if (m_bindlessSampledImageCapacitiesClamped) {
    LOG(WARNING) << fmt::format("VK bindless sampled-image capacities were clamped to device limits ({}). "
                                "If you expected larger tables, adjust the requested flags and/or use a different "
                                "Vulkan device.",
                                support.bindlessClampReason.empty() ? "unknown reason" : support.bindlessClampReason);
  }
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
  CHECK(m_liveDeviceWrapper == nullptr) << "A Vulkan context may have only one live ZVulkanDevice wrapper";
  auto device = std::make_unique<ZVulkanDevice>(*this);
  CHECK(m_liveDeviceWrapper == device.get()) << "Vulkan device wrapper did not register its lifetime with the context";
  return device;
}

void ZVulkanContext::notifyDeviceWrapperCreated(const ZVulkanDevice* device)
{
  CHECK(device != nullptr);
  CHECK(m_liveDeviceWrapper == nullptr) << "A Vulkan context may have only one live ZVulkanDevice wrapper";
  m_liveDeviceWrapper = device;
}

void ZVulkanContext::notifyDeviceWrapperDestroyed(const ZVulkanDevice* device)
{
  CHECK(device != nullptr);
  CHECK(m_liveDeviceWrapper == device) << "Vulkan device-wrapper lifetime callback did not match the context owner";
  m_liveDeviceWrapper = nullptr;
}

} // namespace nim
