#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkantexture.h"
#include "zvulkanshader.h"
#include "zvulkanpipeline.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanframeexecutor.h"
#include <gflags/gflags.h>
#include "zexception.h"
#include "zlog.h"

#include <utility>

namespace nim {

DEFINE_int32(atlas_vk_frames_in_flight, 2, "Max Vulkan frames in flight (debug: set to 1 to serialize submits)");

ZVulkanDevice::ZVulkanDevice(ZVulkanContext& context)
  : m_context(context)
{
  LOG(INFO) << "Vulkan device created";
  // Do not require VK_EXT_vertex_input_dynamic_state (MoltenVK lacks it).
  // Keep the flag false by default; contexts will fall back to fixed VI.

  VmaVulkanFunctions funcs{};
  funcs.vkGetInstanceProcAddr = m_context.context().getDispatcher()->vkGetInstanceProcAddr;
  funcs.vkGetDeviceProcAddr = m_context.instance().getDispatcher()->vkGetDeviceProcAddr;

  VmaAllocatorCreateInfo info{};
  info.instance = *m_context.instance();
  info.physicalDevice = *m_context.physicalDevice();
  info.device = *m_context.device();
  info.pVulkanFunctions = &funcs;
  info.vulkanApiVersion = m_context.physicalDevice().getProperties().apiVersion;
  const VkResult res = vmaCreateAllocator(&info, &m_allocator);
  if (static_cast<vk::Result>(res) != vk::Result::eSuccess) {
    throw ZException("Failed to create VMA allocator");
  }

  // Create tuned VMA pools
  auto createPool = [&](VkMemoryPropertyFlags reqFlags, VkDeviceSize blockSize) -> VmaPool {
    VmaPoolCreateInfo pci{};
    const uint32_t typeIndex = findMemoryTypeIndex(reqFlags);
    if (typeIndex == UINT32_MAX) {
      LOG(ERROR) << "VMA: no memory type for pool flags=" << std::hex << reqFlags;
      return nullptr;
    }
    pci.memoryTypeIndex = typeIndex;
    pci.blockSize = blockSize;
    VmaPool pool = nullptr;
    if (vmaCreatePool(m_allocator, &pci, &pool) != VK_SUCCESS) {
      LOG(ERROR) << "VMA: pool creation failed for type=" << typeIndex;
      return nullptr;
    }
    return pool;
  };
  // Host-visible transient per-frame uploads
  m_uploadTransientPool = createPool(static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible |
                                                                        vk::MemoryPropertyFlagBits::eHostCoherent),
                                     32ull * 1024ull * 1024ull);
  // Host-visible longer-lived staging allocations
  m_uploadStagingPool = createPool(static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible |
                                                                      vk::MemoryPropertyFlagBits::eHostCoherent),
                                   64ull * 1024ull * 1024ull);
  // Device-local static content
  m_deviceLocalPool = createPool(static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eDeviceLocal),
                                 128ull * 1024ull * 1024ull);

  // Log a concise feature/format support summary relevant to Block-ID integer images
  // This helps diagnose sampled vs storage integer image support across stages/drivers.
  if (VLOG_IS_ON(1)) {
    const auto& phys = m_context.physicalDevice();
    const auto features = phys.getFeatures();
    const auto props = phys.getProperties();
    const auto fmt = vk::Format::eR32G32B32A32Uint;
    const auto fprops = phys.getFormatProperties(fmt);
    const auto optimal = fprops.optimalTilingFeatures;
    const bool sampledOK = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eSampledImage);
    const bool storageOK = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eStorageImage);
    const bool sampledLinear = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
    VLOG(1) << "VK Device: 'RGBA32UI' optimal tiling features:"
            << " sampledImage=" << (sampledOK ? 1 : 0) << " storageImage=" << (storageOK ? 1 : 0)
            << " sampledLinear=" << (sampledLinear ? 1 : 0) << " apiVersion=0x" << std::hex << props.apiVersion
            << std::dec;
    VLOG(1) << "VK Features:"
            << " sampledImageArrayDynIdx=" << (features.shaderSampledImageArrayDynamicIndexing ? 1 : 0)
            << " uniformBufArrayDynIdx=" << (features.shaderUniformBufferArrayDynamicIndexing ? 1 : 0)
            << " storageImageExtendedFmt=" << (features.shaderStorageImageExtendedFormats ? 1 : 0)
            << " storageImageReadNoFmt=" << (features.shaderStorageImageReadWithoutFormat ? 1 : 0)
            << " storageImageWriteNoFmt=" << (features.shaderStorageImageWriteWithoutFormat ? 1 : 0);
    // Note: there is no explicit core feature named 'shaderSampledImageInteger'. Integer sampling
    // support is covered by the format's sampledImage bit; stage usage (fragment/compute) is not
    // gated by a separate feature bit.
  }
}

ZVulkanDevice::~ZVulkanDevice()
{
  if (m_uploadTransientPool != nullptr) {
    vmaDestroyPool(m_allocator, m_uploadTransientPool);
    m_uploadTransientPool = nullptr;
  }
  if (m_uploadStagingPool != nullptr) {
    vmaDestroyPool(m_allocator, m_uploadStagingPool);
    m_uploadStagingPool = nullptr;
  }
  if (m_deviceLocalPool != nullptr) {
    vmaDestroyPool(m_allocator, m_deviceLocalPool);
    m_deviceLocalPool = nullptr;
  }
  if (m_allocator != nullptr) {
    vmaDestroyAllocator(m_allocator);
    m_allocator = nullptr;
  }
  LOG(INFO) << "Destroying Vulkan device";
}

std::unique_ptr<ZVulkanBuffer>
ZVulkanDevice::createBuffer(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties)
{
  return std::make_unique<ZVulkanBuffer>(*this, size, usage, properties);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(const ZVulkanTexture::CreateInfo& createInfo)
{
  return std::make_unique<ZVulkanTexture>(*this, createInfo);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width, uint32_t height, vk::Format format)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format);
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width,
                                                             uint32_t height,
                                                             vk::Format format,
                                                             vk::ImageUsageFlags usage,
                                                             vk::MemoryPropertyFlags memoryProperties)
{
  return std::make_unique<ZVulkanTexture>(*this, width, height, format, usage, memoryProperties);
}

std::unique_ptr<ZVulkanShader> ZVulkanDevice::createShader(const std::string& vertexCode,
                                                           const std::string& fragmentCode)
{
  return std::make_unique<ZVulkanShader>(*this, vertexCode, fragmentCode);
}

std::unique_ptr<ZVulkanPipeline>
ZVulkanDevice::createPipeline(ZVulkanShader& shader,
                              const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                              const vk::PrimitiveTopology& topology)
{
  return std::make_unique<ZVulkanPipeline>(*this, shader, vertexInputInfo, topology);
}

ZVulkanFrameExecutor& ZVulkanDevice::frameExecutor()
{
  if (!m_frameExecutor) {
    m_frameExecutor = std::make_unique<ZVulkanFrameExecutor>(*this);
  }
  // Apply debug gflag on each access to allow runtime tweaks.
  const uint32_t fif = std::max<int32_t>(1, FLAGS_atlas_vk_frames_in_flight);
  m_frameExecutor->setMaxFramesInFlight(fif);
  return *m_frameExecutor;
}

const ZVulkanFrameExecutor& ZVulkanDevice::frameExecutor() const
{
  return const_cast<ZVulkanDevice*>(this)->frameExecutor();
}

std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createDescriptorPool()
{
  return std::make_unique<ZVulkanDescriptorPool>(*this);
}

std::unique_ptr<ZVulkanDescriptorSet> ZVulkanDevice::createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                                         vk::DescriptorSetLayout layout,
                                                                         bool isOverrideTransient)
{
  auto descriptorSet = pool.allocateDescriptorSet(layout);
  return std::make_unique<ZVulkanDescriptorSet>(*this, descriptorSet, isOverrideTransient);
}

std::unique_ptr<ZVulkanBuffer> ZVulkanDevice::createBufferInPool(size_t size,
                                                                 vk::BufferUsageFlags usage,
                                                                 vk::MemoryPropertyFlags properties,
                                                                 VmaPool poolOverride)
{
  return std::make_unique<ZVulkanBuffer>(*this, size, usage, properties, poolOverride);
}

uint32_t ZVulkanDevice::findMemoryTypeIndex(VkMemoryPropertyFlags requiredFlags,
                                            VkMemoryPropertyFlags preferredFlags) const
{
  auto& phys = m_context.physicalDevice();
  const auto memProps = phys.getMemoryProperties();
  auto matches = [&](uint32_t i, VkMemoryPropertyFlags req, VkMemoryPropertyFlags pref) {
    VkMemoryPropertyFlags flags = static_cast<VkMemoryPropertyFlags>(memProps.memoryTypes[i].propertyFlags);
    if ((flags & req) != req) {
      return false;
    }
    if (pref != 0 && (flags & pref) != pref) {
      return false;
    }
    return true;
  };
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if (matches(i, requiredFlags, preferredFlags)) {
      return i;
    }
  }
  if (preferredFlags != 0) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
      if (matches(i, requiredFlags, 0)) {
        return i;
      }
    }
  }
  return UINT32_MAX;
}

} // namespace nim
