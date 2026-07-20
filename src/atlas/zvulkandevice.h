#pragma once

#include "zvulkan.h"
#include "zvulkanframeexecutor.h"
#include "zvulkantexture.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nim {

class ZVulkanContext;
class ZVulkanBuffer;
class ZVulkanTexture;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanBindlessDescriptorSet;
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

  std::unique_ptr<ZVulkanPipeline>
  createPipeline(ZVulkanShader& shader,
                 const vk::PipelineVertexInputStateCreateInfo& vertexInputInfo,
                 const vk::PrimitiveTopology& topology = vk::PrimitiveTopology::eTriangleList);

  std::unique_ptr<ZVulkanDescriptorPool> createTransientDescriptorPool();
  std::unique_ptr<ZVulkanDescriptorPool> createPersistentDescriptorPool();
  std::unique_ptr<ZVulkanDescriptorSet> createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                            vk::DescriptorSetLayout layout);

  // Vulkan host access is externally synchronized by Atlas's rendering-thread
  // ownership model. Feature-scoped resource wrappers call this at every
  // mutable pool/device boundary rather than assuming their callers did so.
  void checkOwnerThread(std::string_view operation) const;

  // Device-owned bindless descriptor state. The layout, immutable samplers,
  // placeholders, pool, and one table per frame-executor slot form one lifetime
  // domain and are shared by every Vulkan renderer backend on this device.
  void prepareBindlessDescriptorState();
  [[nodiscard]] vk::DescriptorSetLayout bindlessSampledImageDescriptorSetLayout();
  [[nodiscard]] ZVulkanBindlessDescriptorSet& bindlessSampledImagesForFrame(ZVulkanFrameExecutor::ActiveFrame& frame);
  [[nodiscard]] ZVulkanTexture& defaultBindlessPlaceholderTexture2D();
  [[nodiscard]] bool retireBindlessTexture(const ZVulkanTexture& texture,
                                           const std::shared_ptr<void>& deferredResources);
  void beginBindlessFrameSlot(ZVulkanFrameExecutor::ActiveFrame& frame);
  void drainBindlessRetirements(ZVulkanFrameExecutor::ActiveFrame& frame);
  // Strong-recovery/teardown safe point: wait every submitted executor frame,
  // then replace retired descriptors and release their deferred GPU resources
  // if no unsubmitted command buffer is still recording. Returns whether the
  // all-slot descriptor drain was safe and completed.
  [[nodiscard]] bool waitForAllFramesAndDrainBindlessRetirements();

  [[nodiscard]] uint32_t frameSlotCount() const
  {
    return m_framesInFlight;
  }

  // Read-only diagnostic for the one device-owned bindless pool. Ordinary
  // backend pools cannot reserve update-after-bind descriptors.
  [[nodiscard]] uint64_t updateAfterBindDescriptorsReserved() const;

  // Descriptor writes are prohibited while any backend is recording. This is
  // device-wide because bindless tables are shared across backend instances.
  void beginDescriptorSetRecording(const void* owner);
  void endDescriptorSetRecording(const void* owner);
  [[nodiscard]] bool descriptorSetWritesAllowed() const;

  // Stable object identity for descriptor caches. Image generations cover
  // resource recreation; this identity additionally distinguishes a new
  // texture object allocated at a reused CPU address.
  [[nodiscard]] uint64_t allocateTextureDescriptorIdentity();

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
  // VMA pools: host-visible transient/upload/readback staging and device-local pools
  VmaPool uploadTransientPool() const
  {
    return m_uploadTransientPool;
  }
  VmaPool uploadStagingPool() const
  {
    return m_uploadStagingPool;
  }
  VmaPool readbackStagingPool() const
  {
    return m_readbackStagingPool;
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

  // Reusable host-visible transfer source for immediate texture uploads. Upload
  // commands currently wait for completion before returning, so one device-owned
  // staging buffer can be safely reused across dense texture restores without
  // reallocating a large VMA buffer per channel.
  ZVulkanBuffer& immediateUploadStagingBuffer(size_t size);

private:
  struct BindlessSlotState;

  void ensureBindlessDescriptorSetLayout();
  void ensureBindlessPlaceholderTextures();
  void destroyVmaResources() noexcept;
  [[nodiscard]] ZVulkanBindlessDescriptorSet& bindlessSampledImagesForFrameSlot(uint32_t frameSlot);
  void applyBindlessRetirementsForFrameSlot(uint32_t frameSlot);
  void reserveUpdateAfterBindDescriptors(uint64_t count, std::string_view label);
  void releaseUpdateAfterBindDescriptors(uint64_t count);

  ZVulkanContext& m_context;
  const std::thread::id m_ownerThreadId;
  // ZVulkanContext snapshots this before evaluating the logical device's
  // descriptor budget; sequential wrappers must retain that same topology.
  const uint32_t m_framesInFlight;
  std::unique_ptr<ZVulkanFrameExecutor> m_frameExecutor;
  std::unique_ptr<ZVulkanResidencyManager> m_residencyManager;
  bool m_supportsVertexInputDynamicState = false; // guarded for MoltenVK
  VmaAllocator m_allocator = nullptr;
  VmaPool m_uploadTransientPool = nullptr;
  VmaPool m_uploadStagingPool = nullptr;
  VmaPool m_readbackStagingPool = nullptr;
  VmaPool m_deviceLocalPool = nullptr;
  std::unique_ptr<ZVulkanBuffer> m_immediateUploadStagingBuffer;
  size_t m_immediateUploadStagingBufferSize = 0;
  vk::DeviceSize m_maxMemoryAllocationSize = std::numeric_limits<vk::DeviceSize>::max();

  uint64_t m_updateAfterBindDescriptorsReserved = 0u;
  const void* m_descriptorSetRecordingOwner = nullptr;
  uint64_t m_nextTextureDescriptorIdentity = 1u;

  std::optional<vk::raii::Sampler> m_bindlessLinearClampSampler;
  std::optional<vk::raii::Sampler> m_bindlessNearestClampSampler;
  std::optional<vk::raii::Sampler> m_bindlessLinearBorderZero3DSampler;
  std::optional<vk::raii::DescriptorSetLayout> m_bindlessDescriptorSetLayout;
  std::unique_ptr<ZVulkanTexture> m_bindlessPlaceholder2D;
  std::unique_ptr<ZVulkanTexture> m_bindlessPlaceholder2DArray;
  std::unique_ptr<ZVulkanTexture> m_bindlessPlaceholder3D;
  std::unique_ptr<ZVulkanTexture> m_bindlessPlaceholderU2D;
  std::unique_ptr<ZVulkanTexture> m_bindlessPlaceholderU3D;
  std::optional<vk::raii::DescriptorPool> m_bindlessDescriptorPool;
  uint64_t m_bindlessPoolUpdateAfterBindReservation = 0u;
  std::vector<std::unique_ptr<BindlessSlotState>> m_bindlessSlots;
};

} // namespace nim
