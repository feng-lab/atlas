#pragma once

#include "zvulkan.h"
#include "zvulkantexture.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace nim {

class ZVulkanContext;
class ZVulkanBuffer;
class ZVulkanTexture;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanFrameExecutor;
class ZVulkanResidencyManager;

class ZVulkanDevice
{
public:
  struct DeviceLocalBudget
  {
    uint64_t budgetBytes = 0;
    uint64_t usageBytes = 0;
  };

  explicit ZVulkanDevice(ZVulkanContext& context);
  ~ZVulkanDevice();

  ZVulkanContext& context()
  {
    return m_context;
  }

  std::unique_ptr<ZVulkanBuffer>
  createBuffer(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties);

  std::unique_ptr<ZVulkanTexture> createTexture(const ZVulkanTexture::CreateInfo& createInfo);
  std::unique_ptr<ZVulkanTexture> createTexture(uint32_t width, uint32_t height, vk::Format format);
  std::unique_ptr<ZVulkanTexture> createTexture(uint32_t width,
                                                uint32_t height,
                                                vk::Format format,
                                                vk::ImageUsageFlags usage,
                                                vk::MemoryPropertyFlags memoryProperties);
  void
  reclaimBeforeTextureAllocation(const ZVulkanTexture::CreateInfo& createInfo, bool force, std::string_view reason);
  void enforceTextureAllocationBudgetAfter(const ZVulkanTexture::CreateInfo& createInfo, std::string_view reason);

  std::unique_ptr<ZVulkanShader> createShader(const std::string& vertexCode, const std::string& fragmentCode);

  std::unique_ptr<ZVulkanPipeline>
  createPipeline(ZVulkanShader& shader,
                 const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                 const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);

  std::unique_ptr<ZVulkanDescriptorPool> createDescriptorPool();
  std::unique_ptr<ZVulkanDescriptorSet> createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                            vk::DescriptorSetLayout layout);

  ZVulkanFrameExecutor& frameExecutor();
  const ZVulkanFrameExecutor& frameExecutor() const;

  // Optional features
  bool supportsVertexInputDynamicState() const
  {
    return m_supportsVertexInputDynamicState;
  }

  // Query the best-effort device-local heap budget/usage (via VMA).
  // This is intended for cache residency decisions (e.g., paging caches).
  [[nodiscard]] DeviceLocalBudget deviceLocalBudget() const;

  // Vulkan-only GPU residency manager (host-backed eviction for large caches).
  ZVulkanResidencyManager& residencyManager();
  const ZVulkanResidencyManager& residencyManager() const;

  // Thread-local guard used while the residency broker is reclaiming memory.
  // Reclaim callbacks may need staging buffers (for example host backups for
  // dirty paged-image caches); those allocations must not recursively enter the
  // broker while it already holds residency state.
  void enterAllocationRecoveryScope() const;
  void leaveAllocationRecoveryScope() const;
  [[nodiscard]] bool allocationRecoveryScopeActive() const;

  // VMA allocator handle
  VmaAllocator allocator() const
  {
    return m_allocator;
  }
  // VMA pools: host-visible transient/staging and device-local pools
  VmaPool uploadTransientPool() const
  {
    return m_uploadTransientPool;
  }
  VmaPool uploadStagingPool() const
  {
    return m_uploadStagingPool;
  }
  VmaPool deviceLocalPool() const
  {
    return m_deviceLocalPool;
  }

  [[nodiscard]] vk::DeviceSize maxMemoryAllocationSize() const
  {
    return m_maxMemoryAllocationSize;
  }

  // Utility to query a memory type index with required flags
  uint32_t findMemoryTypeIndex(VkMemoryPropertyFlags requiredFlags, VkMemoryPropertyFlags preferredFlags = 0) const;

  // Optional pool-aware buffer creation
  std::unique_ptr<class ZVulkanBuffer>
  createBufferInPool(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties, VmaPool poolOverride);

private:
  ZVulkanContext& m_context;
  std::unique_ptr<ZVulkanFrameExecutor> m_frameExecutor;
  std::unique_ptr<ZVulkanResidencyManager> m_residencyManager;
  bool m_supportsVertexInputDynamicState = false; // guarded for MoltenVK
  // Captured once at device creation. The frame executor slot ring is keyed by
  // pointer identity, so resizing it at runtime would invalidate keys and can
  // drop fence-gated completion callbacks unless explicitly drained.
  uint32_t m_framesInFlight = 1;
  VmaAllocator m_allocator = nullptr;
  VmaPool m_uploadTransientPool = nullptr;
  VmaPool m_uploadStagingPool = nullptr;
  VmaPool m_deviceLocalPool = nullptr;
  vk::DeviceSize m_maxMemoryAllocationSize = std::numeric_limits<vk::DeviceSize>::max();
};

} // namespace nim
