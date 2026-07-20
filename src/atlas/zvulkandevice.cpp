#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkanresidencymanager.h"
#include "zvulkantexture.h"
#include "zvulkanshader.h"
#include "zvulkanpipeline.h"
#include "zvulkanbindlessdescriptorset.h"
#include "zvulkanbindings.h"
#include "zvulkandescriptorpool.h"
#include "zvulkandescriptorset.h"
#include "zvulkancontext.h"
#include "zvulkanframeexecutor.h"
#include "zexception.h"
#include "zlog.h"

#include <folly/ScopeGuard.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace nim {

struct ZVulkanDevice::BindlessSlotState
{
  std::unique_ptr<ZVulkanBindlessDescriptorSet> descriptorSet;
  std::vector<ZVulkanBindlessDescriptorSet::EntryHandle> pendingRetirements;
  std::vector<std::shared_ptr<void>> pendingRetirementResources;
  uint64_t registrationSerial = 0u;
};

namespace {

thread_local uint32_t s_allocationRecoveryScopeDepth = 0;

ZVulkanResidencyManager::ResourceClass classifyBufferAllocation(vk::BufferUsageFlags usage,
                                                                vk::MemoryPropertyFlags properties,
                                                                VmaPool poolOverride,
                                                                const ZVulkanDevice& device)
{
  if (poolOverride != nullptr && poolOverride == device.uploadTransientPool()) {
    return ZVulkanResidencyManager::ResourceClass::TransientUploadPage;
  }
  if (poolOverride != nullptr && poolOverride == device.readbackStagingPool()) {
    return ZVulkanResidencyManager::ResourceClass::ReadbackStaging;
  }
  if (properties & vk::MemoryPropertyFlagBits::eHostVisible) {
    return ZVulkanResidencyManager::ResourceClass::ReadbackStaging;
  }
  if ((properties & vk::MemoryPropertyFlagBits::eDeviceLocal) &&
      (usage & (vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer))) {
    return ZVulkanResidencyManager::ResourceClass::StaticGeometry;
  }
  return ZVulkanResidencyManager::ResourceClass::PersistentCompositorTarget;
}

ZVulkanResidencyManager::ResourceClass classifyTextureAllocation(const ZVulkanTexture::CreateInfo& createInfo)
{
  switch (createInfo.residencyClassHint) {
    case ZVulkanTexture::ResidencyClassHint::Auto:
      break;
    case ZVulkanTexture::ResidencyClassHint::ScratchBacking:
      return ZVulkanResidencyManager::ResourceClass::ScratchBacking;
    case ZVulkanTexture::ResidencyClassHint::DenseImageTexture:
      return ZVulkanResidencyManager::ResourceClass::DenseImageTexture;
    case ZVulkanTexture::ResidencyClassHint::DenseVolumeTexture:
      return ZVulkanResidencyManager::ResourceClass::DenseVolumeTexture;
    case ZVulkanTexture::ResidencyClassHint::PagedImageMetadataTexture:
      return ZVulkanResidencyManager::ResourceClass::PagedImageMetadataTexture;
    case ZVulkanTexture::ResidencyClassHint::PagedImageCacheR8:
      return ZVulkanResidencyManager::ResourceClass::PagedImageCacheR8;
    case ZVulkanTexture::ResidencyClassHint::PersistentCompositorTarget:
      return ZVulkanResidencyManager::ResourceClass::PersistentCompositorTarget;
  }

  if (createInfo.imageType == vk::ImageType::e3D && createInfo.format == vk::Format::eR8Unorm &&
      (createInfo.usage & vk::ImageUsageFlagBits::eSampled)) {
    return ZVulkanResidencyManager::ResourceClass::DenseVolumeTexture;
  }
  if (createInfo.usage & (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
    return ZVulkanResidencyManager::ResourceClass::PersistentCompositorTarget;
  }
  return ZVulkanResidencyManager::ResourceClass::PersistentCompositorTarget;
}

bool countsAgainstDeviceLocalBudget(vk::MemoryPropertyFlags properties)
{
  return static_cast<bool>(properties & vk::MemoryPropertyFlagBits::eDeviceLocal);
}

[[noreturn]] void throwStrictBudgetExceeded(ZVulkanDevice& device,
                                            ZVulkanResidencyManager::ResourceClass requestClass,
                                            uint64_t requestedBytes,
                                            std::string_view reason,
                                            const ZVulkanResidencyManager::AllocationPressure& pressure)
{
  throw ZException(fmt::format(
    "VK strict residency budget exceeded: class={} requested={}B usage={}B budget={}B unreclaimed={}B reason='{}' memory_by_class=[{}] memory_by_label=[{}]",
    ZVulkanResidencyManager::resourceClassName(requestClass),
    requestedBytes,
    pressure.usageBytes,
    pressure.budgetBytes,
    pressure.reclaimBytes,
    reason.empty() ? "<unspecified>" : std::string(reason),
    device.residencyManager().describeMemoryByClass(),
    device.residencyManager().describeMemoryByLabel()));
}

void brokerReclaimBeforeAllocation(ZVulkanDevice& device,
                                   ZVulkanResidencyManager::ResourceClass requestClass,
                                   uint64_t requestedBytes,
                                   bool enforceDeviceLocalBudget,
                                   bool force,
                                   std::string_view reason)
{
  if (requestedBytes == 0u || device.allocationRecoveryScopeActive()) {
    return;
  }
  const bool strictBudget = device.residencyManager().strictBudgetActive();
  if (!enforceDeviceLocalBudget && !strictBudget) {
    return;
  }
  if (!force &&
      (requestClass == ZVulkanResidencyManager::ResourceClass::ReadbackStaging ||
       requestClass == ZVulkanResidencyManager::ResourceClass::TransientUploadPage) &&
      !strictBudget) {
    // Readback/staging and transient upload buffers are host-visible allocations on the paths that call createBuffer().
    // They stay registered as reclaimable providers, but they should not make a device-local residency budget
    // walk evict sampled images before every small transfer buffer. If the allocation really fails, the
    // force retry below still asks the broker for recovery.
    return;
  }

  const uint64_t strictAllocationBytes =
    strictBudget && !ZVulkanResidencyManager::resourceClassCountsAgainstStrictBudget(requestClass) ? 0u
                                                                                                   : requestedBytes;
  auto pressure = device.residencyManager().allocationPressureFor(strictAllocationBytes);
  if (!force && !pressure.needsReclaim()) {
    return;
  }
  const auto initialPressure = pressure;

  const bool originalForce = force;
  bool waitedForCompletion = false;
  ZVulkanResidencyManager::ReclaimStats totalStats{};
  uint64_t initialReclaimTargetBytes = 0u;
  while (force || pressure.needsReclaim()) {
    const uint64_t reclaimTargetBytes =
      force ? std::max<uint64_t>(requestedBytes, pressure.reclaimBytes) : pressure.reclaimBytes;
    if (initialReclaimTargetBytes == 0u) {
      initialReclaimTargetBytes = reclaimTargetBytes;
    }

    const auto stats = device.residencyManager().reclaimMemory(
      ZVulkanResidencyManager::ReclaimRequest{.requestClass = requestClass,
                                              .requestedBytes = reclaimTargetBytes,
                                              .force = force,
                                              .reason = reason});
    totalStats.add(stats);

    // Released bytes are provider estimates; use the latest driver/broker
    // pressure report as the stop condition so a reclaim pass cannot under-free
    // after allocations with different alignment or residency costs.
    const auto retryPressure = device.residencyManager().allocationPressureFor(strictAllocationBytes);
    if (!retryPressure.needsReclaim()) {
      pressure = retryPressure;
      break;
    }

    if (stats.resourcesReleased == 0u && stats.bytesReleased == 0u) {
      if (!strictBudget) {
        pressure = retryPressure;
        break;
      }
      if (!waitedForCompletion) {
        const uint32_t inFlightBefore = device.frameExecutor().inFlightCount();
        VLOG(1) << fmt::format(
          "VK allocation broker strong recovery: class={} allocation={}B unreclaimed={}B in_flight={} reason='{}'",
          ZVulkanResidencyManager::resourceClassName(requestClass),
          requestedBytes,
          retryPressure.reclaimBytes,
          inFlightBefore,
          reason.empty() ? "<unspecified>" : std::string(reason));
        (void)device.waitForAllFramesAndDrainBindlessRetirements();
        waitedForCompletion = true;
        pressure = device.residencyManager().allocationPressureFor(strictAllocationBytes);
        force = false;
        if (!pressure.needsReclaim()) {
          break;
        }
        continue;
      }
      throwStrictBudgetExceeded(device, requestClass, requestedBytes, reason, retryPressure);
    }
    pressure = retryPressure;
    force = false;
  }

  if (VLOG_IS_ON(1)) {
    const auto afterPressure = device.residencyManager().allocationPressureFor(0u);
    VLOG(1) << fmt::format(
      "VK allocation broker reclaim: class={} allocation={}B target={}B force={} released={} resources={} usage_before={}B budget={}B usage_after={}B reason='{}'",
      ZVulkanResidencyManager::resourceClassName(requestClass),
      requestedBytes,
      initialReclaimTargetBytes,
      originalForce ? 1 : 0,
      totalStats.bytesReleased,
      totalStats.resourcesReleased,
      initialPressure.usageBytes,
      initialPressure.budgetBytes,
      afterPressure.usageBytes,
      reason.empty() ? "<unspecified>" : std::string(reason));
  }
}

void enforceStrictBudgetAfterAllocation(ZVulkanDevice& device,
                                        ZVulkanResidencyManager::ResourceClass requestClass,
                                        uint64_t requestedBytes,
                                        bool enforceDeviceLocalBudget,
                                        std::string_view reason)
{
  if (!device.residencyManager().strictBudgetActive()) {
    return;
  }
  if (device.allocationRecoveryScopeActive()) {
    return;
  }
  (void)enforceDeviceLocalBudget;

  auto pressure = device.residencyManager().allocationPressureFor(0u);
  if (!pressure.needsReclaim()) {
    return;
  }

  ZVulkanResidencyManager::ReclaimStats totalStats{};
  bool waitedForCompletion = false;
  const auto initialPressure = pressure;
  while (pressure.needsReclaim()) {
    const uint64_t reclaimTargetBytes = pressure.reclaimBytes;
    const auto stats = device.residencyManager().reclaimMemory(
      ZVulkanResidencyManager::ReclaimRequest{.requestClass = requestClass,
                                              .requestedBytes = reclaimTargetBytes,
                                              .force = false,
                                              .reason = reason});
    totalStats.add(stats);

    pressure = device.residencyManager().allocationPressureFor(0u);
    if (!pressure.needsReclaim()) {
      VLOG(1) << fmt::format(
        "VK post-allocation broker reclaim: class={} allocation={}B target={}B released={} resources={} usage_before={}B usage_after={}B budget={}B reason='{}'",
        ZVulkanResidencyManager::resourceClassName(requestClass),
        requestedBytes,
        reclaimTargetBytes,
        totalStats.bytesReleased,
        totalStats.resourcesReleased,
        initialPressure.usageBytes,
        pressure.usageBytes,
        initialPressure.budgetBytes,
        reason.empty() ? "<unspecified>" : std::string(reason));
      return;
    }

    if (stats.resourcesReleased != 0u || stats.bytesReleased != 0u) {
      continue;
    }
    if (!waitedForCompletion) {
      const uint32_t inFlightBefore = device.frameExecutor().inFlightCount();
      VLOG(1) << fmt::format(
        "VK post-allocation broker strong recovery: class={} allocation={}B unreclaimed={}B in_flight={} reason='{}'",
        ZVulkanResidencyManager::resourceClassName(requestClass),
        requestedBytes,
        pressure.reclaimBytes,
        inFlightBefore,
        reason.empty() ? "<unspecified>" : std::string(reason));
      (void)device.waitForAllFramesAndDrainBindlessRetirements();
      waitedForCompletion = true;
      pressure = device.residencyManager().allocationPressureFor(0u);
      if (!pressure.needsReclaim()) {
        return;
      }
      continue;
    }
    break;
  }

  pressure.allocationBytes = requestedBytes;
  throwStrictBudgetExceeded(device, requestClass, requestedBytes, reason, pressure);
}

void logAllocationFailureReport(ZVulkanDevice& device,
                                ZVulkanResidencyManager::ResourceClass requestClass,
                                uint64_t requestedBytes,
                                std::string_view reason,
                                const std::exception& e)
{
  const auto budget = device.deviceLocalBudget();
  LOG(ERROR) << fmt::format(
    "VK allocation failed after broker reclaim: class={} requested={}B usage={}B budget={}B reason='{}' error={} memory_by_class=[{}] memory_by_label=[{}]",
    ZVulkanResidencyManager::resourceClassName(requestClass),
    requestedBytes,
    budget.usageBytes,
    budget.budgetBytes,
    reason.empty() ? "<unspecified>" : std::string(reason),
    e.what(),
    device.residencyManager().describeMemoryByClass(),
    device.residencyManager().describeMemoryByLabel());
}

} // namespace

ZVulkanDevice::ZVulkanDevice(ZVulkanContext& context)
  : m_context(context)
  , m_ownerThreadId(std::this_thread::get_id())
  , m_framesInFlight(context.frameSlotCount())
{
  CHECK_GT(m_framesInFlight, 0u) << "Vulkan context resolved an invalid frame-slot count";

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
  // Enable VK_EXT_memory_budget integration when available so we can make
  // best-effort cache residency decisions without hardcoding VRAM caps.
  if (m_context.supportsMemoryBudget()) {
    info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  }
  const VkResult res = vmaCreateAllocator(&info, &m_allocator);
  if (static_cast<vk::Result>(res) != vk::Result::eSuccess) {
    throw ZException("Failed to create VMA allocator");
  }
  auto allocatorCleanupGuard = folly::makeGuard([this]() noexcept {
    // Raw VMA handles are not RAII members. Keep constructor failure
    // transactional if a later pool, broker, query, or logging allocation
    // throws before the wrapper is published to ZVulkanContext.
    m_residencyManager.reset();
    m_immediateUploadStagingBuffer.reset();
    destroyVmaResources();
  });

  // Create tuned VMA pools
  auto createPool = [&](VkMemoryPropertyFlags reqFlags, VkDeviceSize blockSize, std::string_view label) -> VmaPool {
    VmaPoolCreateInfo pci{};
    const uint32_t typeIndex = findMemoryTypeIndex(reqFlags);
    if (typeIndex == UINT32_MAX) {
      LOG(ERROR) << fmt::format("VMA: no memory type for {} pool flags=0x{:x}", label, reqFlags);
      return nullptr;
    }
    pci.memoryTypeIndex = typeIndex;
    pci.blockSize = blockSize;
    VmaPool pool = nullptr;
    if (vmaCreatePool(m_allocator, &pci, &pool) != VK_SUCCESS) {
      LOG(ERROR) << "VMA: " << label << " pool creation failed for type=" << typeIndex;
      return nullptr;
    }
    return pool;
  };
  const auto hostVisibleCoherent = static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible |
                                                                      vk::MemoryPropertyFlagBits::eHostCoherent);
  const auto hostVisibleCoherentCached = static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eHostVisible |
                                                                            vk::MemoryPropertyFlagBits::eHostCoherent |
                                                                            vk::MemoryPropertyFlagBits::eHostCached);
  // Host-visible transient per-frame uploads
  m_uploadTransientPool = createPool(hostVisibleCoherent, 32ull * 1024ull * 1024ull, "upload transient");
  // Host-visible longer-lived staging allocations
  m_uploadStagingPool = createPool(hostVisibleCoherent, 64ull * 1024ull * 1024ull, "upload staging");
  // GPU-to-CPU readback is CPU-read-heavy. Prefer cached coherent host memory,
  // but keep coherent-only as the portable fallback so mapped reads stay valid
  // without explicit invalidate calls.
  if (findMemoryTypeIndex(hostVisibleCoherentCached) != UINT32_MAX) {
    m_readbackStagingPool = createPool(hostVisibleCoherentCached, 64ull * 1024ull * 1024ull, "readback staging");
  } else {
    LOG(INFO) << "VMA: HOST_CACHED coherent readback staging memory unavailable; using coherent readback staging";
    m_readbackStagingPool = createPool(hostVisibleCoherent, 64ull * 1024ull * 1024ull, "readback staging fallback");
  }
  // Device-local static content
  m_deviceLocalPool = createPool(static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlagBits::eDeviceLocal),
                                 128ull * 1024ull * 1024ull,
                                 "device local");

  // Residency manager (device-owned, Vulkan-only).
  m_residencyManager = std::make_unique<ZVulkanResidencyManager>(*this);

  // Log a concise feature/format support summary relevant to Block-ID integer images
  // One-time device limits + calibrated timestamps summary for visibility
  const auto& phys = m_context.physicalDevice();
  const auto props = phys.getProperties();
  const auto& limits = props.limits;
  try {
    auto props2 = phys.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMaintenance3Properties>();
    m_maxMemoryAllocationSize = props2.get<vk::PhysicalDeviceMaintenance3Properties>().maxMemoryAllocationSize;
  }
  catch (...) {
    m_maxMemoryAllocationSize = std::numeric_limits<vk::DeviceSize>::max();
  }
  const auto features = phys.getFeatures();
  const auto fmt = vk::Format::eR32G32B32A32Uint;
  const auto fprops = phys.getFormatProperties(fmt);
  const auto optimal = fprops.optimalTilingFeatures;
  const bool sampledOK = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eSampledImage);
  const bool storageOK = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eStorageImage);
  const bool sampledLinear = static_cast<bool>(optimal & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
  LOG(INFO) << fmt::format(
    "VK device features: api=0x{:x} sampledImageArrayDynIdx={} uniformBufArrayDynIdx={} storageImageExtendedFmt={} storageImageReadNoFmt={} storageImageWriteNoFmt={} RGBA32UI(sampled={},storage={},linear={})",
    props.apiVersion,
    (features.shaderSampledImageArrayDynamicIndexing ? 1 : 0),
    (features.shaderUniformBufferArrayDynamicIndexing ? 1 : 0),
    (features.shaderStorageImageExtendedFormats ? 1 : 0),
    (features.shaderStorageImageReadWithoutFormat ? 1 : 0),
    (features.shaderStorageImageWriteWithoutFormat ? 1 : 0),
    (sampledOK ? 1 : 0),
    (storageOK ? 1 : 0),
    (sampledLinear ? 1 : 0));
  const size_t minAlign = static_cast<size_t>(limits.minUniformBufferOffsetAlignment);
  const size_t maxRange = static_cast<size_t>(limits.maxUniformBufferRange);
  LOG(INFO) << fmt::format(
    "VK device uniform limits: minUniformBufferOffsetAlignment={}B maxUniformBufferRange={}B framesInFlight={}",
    (minAlign ? minAlign : static_cast<size_t>(256)),
    maxRange,
    m_framesInFlight);
  LOG(INFO) << fmt::format("VK device allocation limits: maxMemoryAllocationSize={}B",
                           static_cast<uint64_t>(m_maxMemoryAllocationSize));
  // This extension query is logging-only, and some Windows loader/ICD combinations fail-fast
  // instead of throwing when the entry point is called without device support.
  if (m_context.supportsCalibratedTimestamps()) {
    try {
      auto domains = phys.getCalibrateableTimeDomainsEXT();
      bool supported = !domains.empty();
      std::string domList;
      for (size_t i = 0; i < domains.size(); ++i) {
        domList += fmt::format("{}", enumOrUnderlying(domains[i], 16));
        if (i + 1 < domains.size()) {
          domList += ",";
        }
      }
      const float ts = (props.limits.timestampPeriod > 0.0f) ? props.limits.timestampPeriod : 1.0f;
      LOG(INFO) << "VK calibrated timestamps: supported=" << supported << " domains=[" << domList
                << "] timestampPeriod=" << ts << " ns/tick";
    }
    catch (...) {
      const float ts = (props.limits.timestampPeriod > 0.0f) ? props.limits.timestampPeriod : 1.0f;
      LOG(INFO) << "VK calibrated timestamps: query failed; timestampPeriod=" << ts << " ns/tick";
    }
  } else {
    const float ts = (props.limits.timestampPeriod > 0.0f) ? props.limits.timestampPeriod : 1.0f;
    LOG(INFO) << "VK calibrated timestamps: extension not available; timestampPeriod=" << ts << " ns/tick";
  }
  m_context.notifyDeviceWrapperCreated(this);
  allocatorCleanupGuard.dismiss();
}

ZVulkanDevice::~ZVulkanDevice()
{
  checkOwnerThread("destroy Vulkan device wrapper");
  CHECK(descriptorSetWritesAllowed()) << "Destroying Vulkan device while a backend is recording commands";
  // Flush fence-gated callbacks, sanitize retired descriptors, and release
  // their deferred GPU handles before tearing down the allocator.
  CHECK(waitForAllFramesAndDrainBindlessRetirements())
    << "Vulkan device teardown found an unsubmitted command buffer still recording";

  // The bindless tables own raw descriptor-set handles allocated from their
  // pools. Tear down in dependency order while the logical device, residency
  // manager, and VMA allocator are all still alive.
  m_bindlessSlots.clear();
  m_bindlessDescriptorPool.reset();
  if (m_bindlessPoolUpdateAfterBindReservation != 0u) {
    releaseUpdateAfterBindDescriptors(m_bindlessPoolUpdateAfterBindReservation);
    m_bindlessPoolUpdateAfterBindReservation = 0u;
  }
  CHECK_EQ(updateAfterBindDescriptorsReserved(), 0u)
    << "Update-after-bind descriptor reservation leaked during device teardown";
  m_bindlessPlaceholderU3D.reset();
  m_bindlessPlaceholderU2D.reset();
  m_bindlessPlaceholder3D.reset();
  m_bindlessPlaceholder2DArray.reset();
  m_bindlessPlaceholder2D.reset();
  m_bindlessDescriptorSetLayout.reset();
  m_bindlessLinearBorderZero3DSampler.reset();
  m_bindlessNearestClampSampler.reset();
  m_bindlessLinearClampSampler.reset();

  // Managed textures owned by the residency manager destroy VkImages via VMA,
  // so they must be released before the allocator/pools are torn down.
  m_residencyManager.reset();
  m_frameExecutor.reset();
  m_immediateUploadStagingBuffer.reset();
  m_immediateUploadStagingBufferSize = 0;

  destroyVmaResources();
  m_context.notifyDeviceWrapperDestroyed(this);
  LOG(INFO) << "Destroying Vulkan device";
}

void ZVulkanDevice::destroyVmaResources() noexcept
{
  if (m_uploadTransientPool != nullptr) {
    vmaDestroyPool(m_allocator, m_uploadTransientPool);
    m_uploadTransientPool = nullptr;
  }
  if (m_uploadStagingPool != nullptr) {
    vmaDestroyPool(m_allocator, m_uploadStagingPool);
    m_uploadStagingPool = nullptr;
  }
  if (m_readbackStagingPool != nullptr) {
    vmaDestroyPool(m_allocator, m_readbackStagingPool);
    m_readbackStagingPool = nullptr;
  }
  if (m_deviceLocalPool != nullptr) {
    vmaDestroyPool(m_allocator, m_deviceLocalPool);
    m_deviceLocalPool = nullptr;
  }
  if (m_allocator != nullptr) {
    vmaDestroyAllocator(m_allocator);
    m_allocator = nullptr;
  }
}

std::unique_ptr<ZVulkanBuffer>
ZVulkanDevice::createBuffer(size_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties)
{
  const auto requestClass = classifyBufferAllocation(usage, properties, VK_NULL_HANDLE, *this);
  const bool enforceBudget = countsAgainstDeviceLocalBudget(properties);
  brokerReclaimBeforeAllocation(*this, requestClass, size, enforceBudget, false, "buffer_preallocate");
  try {
    auto buffer = std::make_unique<ZVulkanBuffer>(*this, size, usage, properties);
    enforceStrictBudgetAfterAllocation(*this, requestClass, size, enforceBudget, "buffer_postallocate");
    return buffer;
  }
  catch (const std::exception&) {
    brokerReclaimBeforeAllocation(*this, requestClass, size, enforceBudget, true, "buffer_allocation_retry");
    try {
      auto buffer = std::make_unique<ZVulkanBuffer>(*this, size, usage, properties);
      enforceStrictBudgetAfterAllocation(*this, requestClass, size, enforceBudget, "buffer_retry_postallocate");
      return buffer;
    }
    catch (const std::exception& retryError) {
      logAllocationFailureReport(*this, requestClass, size, "buffer_allocation_retry", retryError);
      throw;
    }
  }
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(const ZVulkanTexture::CreateInfo& createInfo)
{
  const uint64_t requestedBytes = ZVulkanTexture::estimateImageBytes(createInfo);
  const auto requestClass = classifyTextureAllocation(createInfo);
  reclaimBeforeTextureAllocation(createInfo, false, "image_preallocate");
  try {
    auto texture = std::make_unique<ZVulkanTexture>(*this, createInfo);
    enforceTextureAllocationBudgetAfter(createInfo, "image_postallocate");
    return texture;
  }
  catch (const std::exception&) {
    reclaimBeforeTextureAllocation(createInfo, true, "image_allocation_retry");
    try {
      auto texture = std::make_unique<ZVulkanTexture>(*this, createInfo);
      enforceTextureAllocationBudgetAfter(createInfo, "image_retry_postallocate");
      return texture;
    }
    catch (const std::exception& retryError) {
      logAllocationFailureReport(*this, requestClass, requestedBytes, "image_allocation_retry", retryError);
      throw;
    }
  }
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width, uint32_t height, vk::Format format)
{
  return createTexture(ZVulkanTexture::CreateInfo::make2D(width, height, format));
}

std::unique_ptr<ZVulkanTexture> ZVulkanDevice::createTexture(uint32_t width,
                                                             uint32_t height,
                                                             vk::Format format,
                                                             vk::ImageUsageFlags usage,
                                                             vk::MemoryPropertyFlags memoryProperties)
{
  return createTexture(ZVulkanTexture::CreateInfo::make2D(width, height, format, usage, memoryProperties));
}

void ZVulkanDevice::reclaimBeforeTextureAllocation(const ZVulkanTexture::CreateInfo& createInfo,
                                                   bool force,
                                                   std::string_view reason)
{
  const uint64_t requestedBytes = ZVulkanTexture::estimateImageBytes(createInfo);
  const auto requestClass = classifyTextureAllocation(createInfo);
  const bool enforceBudget = countsAgainstDeviceLocalBudget(createInfo.memoryProperties);
  brokerReclaimBeforeAllocation(*this, requestClass, requestedBytes, enforceBudget, force, reason);
}

void ZVulkanDevice::enforceTextureAllocationBudgetAfter(const ZVulkanTexture::CreateInfo& createInfo,
                                                        std::string_view reason)
{
  const uint64_t requestedBytes = ZVulkanTexture::estimateImageBytes(createInfo);
  const auto requestClass = classifyTextureAllocation(createInfo);
  const bool enforceBudget = countsAgainstDeviceLocalBudget(createInfo.memoryProperties);
  enforceStrictBudgetAfterAllocation(*this, requestClass, requestedBytes, enforceBudget, reason);
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
  checkOwnerThread("access frame executor");
  if (!m_frameExecutor) {
    m_frameExecutor = std::make_unique<ZVulkanFrameExecutor>(*this, m_framesInFlight);
  }
  CHECK(m_frameExecutor->maxFramesInFlight() == m_framesInFlight)
    << "Vulkan frame executor frames-in-flight changed unexpectedly";
  return *m_frameExecutor;
}

const ZVulkanFrameExecutor& ZVulkanDevice::frameExecutor() const
{
  return const_cast<ZVulkanDevice*>(this)->frameExecutor();
}

ZVulkanResidencyManager& ZVulkanDevice::residencyManager()
{
  CHECK(m_residencyManager != nullptr) << "Vulkan residency manager missing";
  return *m_residencyManager;
}

const ZVulkanResidencyManager& ZVulkanDevice::residencyManager() const
{
  CHECK(m_residencyManager != nullptr) << "Vulkan residency manager missing";
  return *m_residencyManager;
}

void ZVulkanDevice::enterAllocationRecoveryScope() const
{
  CHECK(s_allocationRecoveryScopeDepth < std::numeric_limits<uint32_t>::max())
    << "Vulkan allocation recovery scope depth overflow";
  s_allocationRecoveryScopeDepth++;
}

void ZVulkanDevice::leaveAllocationRecoveryScope() const
{
  CHECK(s_allocationRecoveryScopeDepth > 0u) << "Vulkan allocation recovery scope underflow";
  s_allocationRecoveryScopeDepth--;
}

bool ZVulkanDevice::allocationRecoveryScopeActive() const
{
  return s_allocationRecoveryScopeDepth > 0u;
}

void ZVulkanDevice::checkOwnerThread(std::string_view operation) const
{
  CHECK(std::this_thread::get_id() == m_ownerThreadId)
    << "Vulkan device operation must run on its owning rendering thread: " << operation;
}

void ZVulkanDevice::reserveUpdateAfterBindDescriptors(uint64_t count, std::string_view label)
{
  checkOwnerThread("reserve update-after-bind descriptors");
  CHECK_GT(count, 0u) << "Update-after-bind reservation must be non-zero";
  const uint64_t limit = m_context.selectedDeviceSupport().maxUpdateAfterBindDescriptorsInAllPools;
  CHECK_LE(m_updateAfterBindDescriptorsReserved, limit);
  CHECK_LE(count, limit - m_updateAfterBindDescriptorsReserved)
    << "Update-after-bind descriptor budget exceeded while reserving " << label << ": requested=" << count
    << " already_reserved=" << m_updateAfterBindDescriptorsReserved << " device_limit=" << limit;
  m_updateAfterBindDescriptorsReserved += count;
  VLOG(1) << "VK update-after-bind reservation: label=" << label << " added=" << count
          << " total=" << m_updateAfterBindDescriptorsReserved << "/" << limit;
}

void ZVulkanDevice::releaseUpdateAfterBindDescriptors(uint64_t count)
{
  checkOwnerThread("release update-after-bind descriptors");
  CHECK_GT(count, 0u) << "Update-after-bind release must be non-zero";
  CHECK_LE(count, m_updateAfterBindDescriptorsReserved)
    << "Update-after-bind descriptor reservation underflow: released=" << count
    << " reserved=" << m_updateAfterBindDescriptorsReserved;
  m_updateAfterBindDescriptorsReserved -= count;
}

uint64_t ZVulkanDevice::updateAfterBindDescriptorsReserved() const
{
  checkOwnerThread("query update-after-bind descriptor reservation");
  return m_updateAfterBindDescriptorsReserved;
}

void ZVulkanDevice::beginDescriptorSetRecording(const void* owner)
{
  checkOwnerThread("begin descriptor-set recording");
  CHECK(owner != nullptr) << "Descriptor recording owner must be non-null";
  CHECK(m_descriptorSetRecordingOwner == nullptr)
    << "A second Vulkan backend attempted to record while another backend owns the device descriptor guard";
  m_descriptorSetRecordingOwner = owner;
}

void ZVulkanDevice::endDescriptorSetRecording(const void* owner)
{
  checkOwnerThread("end descriptor-set recording");
  CHECK(owner != nullptr) << "Descriptor recording owner must be non-null";
  CHECK(m_descriptorSetRecordingOwner == owner)
    << "Vulkan descriptor recording guard released by a backend that does not own it";
  m_descriptorSetRecordingOwner = nullptr;
}

bool ZVulkanDevice::descriptorSetWritesAllowed() const
{
  checkOwnerThread("query descriptor-set write guard");
  return m_descriptorSetRecordingOwner == nullptr;
}

uint64_t ZVulkanDevice::allocateTextureDescriptorIdentity()
{
  checkOwnerThread("allocate texture descriptor identity");
  const uint64_t identity = m_nextTextureDescriptorIdentity++;
  CHECK_NE(identity, 0u) << "Vulkan texture descriptor identity wrapped";
  CHECK_NE(identity, std::numeric_limits<uint64_t>::max()) << "Vulkan texture descriptor identity exhausted";
  return identity;
}

void ZVulkanDevice::ensureBindlessDescriptorSetLayout()
{
  checkOwnerThread("ensure bindless descriptor-set layout");
  if (m_bindlessDescriptorSetLayout.has_value()) {
    return;
  }
  CHECK(descriptorSetWritesAllowed()) << "Creating bindless descriptor layout while a backend is recording";

  auto& vkDevice = m_context.device();
  vk::SamplerCreateInfo linearClampInfo{.magFilter = vk::Filter::eLinear,
                                        .minFilter = vk::Filter::eLinear,
                                        .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                        .borderColor = vk::BorderColor::eFloatOpaqueWhite};
  m_bindlessLinearClampSampler.emplace(vkDevice, linearClampInfo);

  vk::SamplerCreateInfo nearestClampInfo{.magFilter = vk::Filter::eNearest,
                                         .minFilter = vk::Filter::eNearest,
                                         .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                         .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                         .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                         .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                         .borderColor = vk::BorderColor::eFloatOpaqueWhite};
  m_bindlessNearestClampSampler.emplace(vkDevice, nearestClampInfo);

  vk::SamplerCreateInfo linearBorderZeroInfo{.magFilter = vk::Filter::eLinear,
                                             .minFilter = vk::Filter::eLinear,
                                             .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                             .addressModeU = vk::SamplerAddressMode::eClampToBorder,
                                             .addressModeV = vk::SamplerAddressMode::eClampToBorder,
                                             .addressModeW = vk::SamplerAddressMode::eClampToBorder,
                                             .borderColor = vk::BorderColor::eFloatTransparentBlack};
  m_bindlessLinearBorderZero3DSampler.emplace(vkDevice, linearBorderZeroInfo);

  const auto& caps = m_context.effectiveBindlessSampledImageCapacities();
  const vk::ShaderStageFlags fragmentStage = vk::ShaderStageFlagBits::eFragment;
  const vk::ShaderStageFlags computeStage = vk::ShaderStageFlagBits::eCompute;
  const std::array<vk::Sampler, 1> linearSamplers{**m_bindlessLinearClampSampler};
  const std::array<vk::Sampler, 1> nearestSamplers{**m_bindlessNearestClampSampler};
  const std::array<vk::Sampler, 1> linearBorderZeroSamplers{**m_bindlessLinearBorderZero3DSampler};
  const std::array<vk::DescriptorSetLayoutBinding, 8> bindings{
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture2D,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .descriptorCount = caps.texture2D,
                                   .stageFlags = fragmentStage},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture2DArray,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .descriptorCount = caps.texture2DArray,
                                   .stageFlags = fragmentStage},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessTexture3D,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .descriptorCount = caps.texture3D,
                                   .stageFlags = fragmentStage},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessUTexture2D,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .descriptorCount = caps.uTexture2D,
                                   .stageFlags = computeStage},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessUTexture3D,
                                   .descriptorType = vk::DescriptorType::eSampledImage,
                                   .descriptorCount = caps.uTexture3D,
                                   .stageFlags = fragmentStage},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerLinearClamp,
                                   .descriptorType = vk::DescriptorType::eSampler,
                                   .descriptorCount = 1u,
                                   .stageFlags = fragmentStage,
                                   .pImmutableSamplers = linearSamplers.data()},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerNearestClamp,
                                   .descriptorType = vk::DescriptorType::eSampler,
                                   .descriptorCount = 1u,
                                   .stageFlags = fragmentStage | computeStage,
                                   .pImmutableSamplers = nearestSamplers.data()},
    vk::DescriptorSetLayoutBinding{.binding = vkbind::kBindingBindlessSamplerLinearBorderZero3D,
                                   .descriptorType = vk::DescriptorType::eSampler,
                                   .descriptorCount = 1u,
                                   .stageFlags = fragmentStage,
                                   .pImmutableSamplers = linearBorderZeroSamplers.data()},
  };

  const bool useUpdateAfterBind = m_context.supportsDescriptorIndexingSampledImageUpdateAfterBind();
  std::array<vk::DescriptorBindingFlags, 8> bindingFlags{};
  for (size_t i = 0; i < 5u; ++i) {
    bindingFlags[i] = vk::DescriptorBindingFlagBits::ePartiallyBound;
    if (useUpdateAfterBind) {
      bindingFlags[i] |= vk::DescriptorBindingFlagBits::eUpdateAfterBind;
    }
  }
  vk::DescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
    .bindingCount = static_cast<uint32_t>(bindingFlags.size()),
    .pBindingFlags = bindingFlags.data(),
  };
  vk::DescriptorSetLayoutCreateFlags layoutFlags{};
  if (useUpdateAfterBind) {
    layoutFlags |= vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
  }
  vk::DescriptorSetLayoutCreateInfo layoutInfo{
    .pNext = &flagsInfo,
    .flags = layoutFlags,
    .bindingCount = static_cast<uint32_t>(bindings.size()),
    .pBindings = bindings.data(),
  };
  m_bindlessDescriptorSetLayout.emplace(vkDevice, layoutInfo);
}

void ZVulkanDevice::ensureBindlessPlaceholderTextures()
{
  checkOwnerThread("ensure bindless placeholder textures");
  if (m_bindlessPlaceholder2D != nullptr) {
    CHECK(m_bindlessPlaceholder2DArray != nullptr);
    CHECK(m_bindlessPlaceholder3D != nullptr);
    CHECK(m_bindlessPlaceholderU2D != nullptr);
    CHECK(m_bindlessPlaceholderU3D != nullptr);
    return;
  }
  CHECK(descriptorSetWritesAllowed()) << "Creating bindless placeholders while a backend is recording";

  constexpr vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
  constexpr vk::MemoryPropertyFlags memory = vk::MemoryPropertyFlagBits::eDeviceLocal;

  auto info2D = ZVulkanTexture::CreateInfo::make2D(1u,
                                                   1u,
                                                   vk::Format::eR8G8B8A8Unorm,
                                                   usage,
                                                   memory,
                                                   1u,
                                                   false,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
  auto placeholder2D = createTexture(info2D);
  const uint32_t white = 0xffffffffu;
  placeholder2D->uploadData(&white, sizeof(white), vk::ImageLayout::eShaderReadOnlyOptimal);

  auto info2DArray = ZVulkanTexture::CreateInfo::make2DArray(1u,
                                                             1u,
                                                             1u,
                                                             vk::Format::eR8G8B8A8Unorm,
                                                             usage,
                                                             memory,
                                                             1u,
                                                             false,
                                                             vk::ImageLayout::eShaderReadOnlyOptimal);
  auto placeholder2DArray = createTexture(info2DArray);
  placeholder2DArray->uploadData(&white, sizeof(white), vk::ImageLayout::eShaderReadOnlyOptimal);

  auto info3D = ZVulkanTexture::CreateInfo::make3D(1u,
                                                   1u,
                                                   1u,
                                                   vk::Format::eR8Unorm,
                                                   usage,
                                                   memory,
                                                   1u,
                                                   false,
                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
  auto placeholder3D = createTexture(info3D);
  const uint8_t zeroByte = 0u;
  placeholder3D->uploadData(&zeroByte, sizeof(zeroByte), vk::ImageLayout::eShaderReadOnlyOptimal);

  auto infoU2D = ZVulkanTexture::CreateInfo::make2D(1u,
                                                    1u,
                                                    vk::Format::eR32G32B32A32Uint,
                                                    usage,
                                                    memory,
                                                    1u,
                                                    false,
                                                    vk::ImageLayout::eShaderReadOnlyOptimal);
  auto placeholderU2D = createTexture(infoU2D);
  const std::array<uint32_t, 4> zeroUint{0u, 0u, 0u, 0u};
  placeholderU2D->uploadData(zeroUint.data(), sizeof(zeroUint), vk::ImageLayout::eShaderReadOnlyOptimal);

  auto infoU3D = ZVulkanTexture::CreateInfo::make3D(1u,
                                                    1u,
                                                    1u,
                                                    vk::Format::eR32G32B32A32Uint,
                                                    usage,
                                                    memory,
                                                    1u,
                                                    false,
                                                    vk::ImageLayout::eShaderReadOnlyOptimal);
  auto placeholderU3D = createTexture(infoU3D);
  placeholderU3D->uploadData(zeroUint.data(), sizeof(zeroUint), vk::ImageLayout::eShaderReadOnlyOptimal);

  m_bindlessPlaceholder2D = std::move(placeholder2D);
  m_bindlessPlaceholder2DArray = std::move(placeholder2DArray);
  m_bindlessPlaceholder3D = std::move(placeholder3D);
  m_bindlessPlaceholderU2D = std::move(placeholderU2D);
  m_bindlessPlaceholderU3D = std::move(placeholderU3D);
}

void ZVulkanDevice::prepareBindlessDescriptorState()
{
  checkOwnerThread("prepare bindless descriptor state");
  if (!m_bindlessSlots.empty()) {
    CHECK_EQ(m_bindlessSlots.size(), static_cast<size_t>(m_framesInFlight));
    return;
  }
  CHECK(descriptorSetWritesAllowed()) << "Preparing bindless descriptors while a backend is recording";
  ensureBindlessDescriptorSetLayout();
  ensureBindlessPlaceholderTextures();

  const auto& caps = m_context.effectiveBindlessSampledImageCapacities();
  const uint64_t sampledImagesPerSet = caps.totalSampledImages();
  CHECK_GT(sampledImagesPerSet, 0u) << "Bindless capacities not computed before descriptor pool creation";
  CHECK_LE(sampledImagesPerSet, std::numeric_limits<uint64_t>::max() / m_framesInFlight)
    << "Bindless sampled-image pool sizing overflow";
  const uint64_t sampledImages64 = sampledImagesPerSet * m_framesInFlight;
  const uint64_t samplers64 =
    static_cast<uint64_t>(ZVulkanDeviceSupport::DescriptorPoolPolicy::kBindlessSamplerDescriptors) * m_framesInFlight;
  CHECK_LE(sampledImages64, std::numeric_limits<uint32_t>::max())
    << "Device bindless sampled-image pool exceeds Vulkan's uint32 descriptor count";
  CHECK_LE(samplers64, std::numeric_limits<uint32_t>::max())
    << "Device bindless sampler pool exceeds Vulkan's uint32 descriptor count";
  const std::array<vk::DescriptorPoolSize, 2> poolSizes{
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampledImage,
                           .descriptorCount = static_cast<uint32_t>(sampledImages64)                                      },
    vk::DescriptorPoolSize{.type = vk::DescriptorType::eSampler,      .descriptorCount = static_cast<uint32_t>(samplers64)},
  };
  vk::DescriptorPoolCreateFlags poolFlags{};
  uint64_t updateAfterBindReservation = 0u;
  if (m_context.supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
    poolFlags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
    updateAfterBindReservation = sampledImages64 + samplers64;
    reserveUpdateAfterBindDescriptors(updateAfterBindReservation, "device_bindless_pool");
  }

  std::optional<vk::raii::DescriptorPool> descriptorPool;
  std::vector<std::unique_ptr<BindlessSlotState>> slots;
  try {
    vk::DescriptorPoolCreateInfo poolInfo{.flags = poolFlags,
                                          .maxSets = m_framesInFlight,
                                          .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
                                          .pPoolSizes = poolSizes.data()};
    descriptorPool.emplace(m_context.device(), poolInfo);

    std::vector<vk::DescriptorSetLayout> layouts(m_framesInFlight, **m_bindlessDescriptorSetLayout);
    vk::DescriptorSetAllocateInfo allocateInfo{.descriptorPool = **descriptorPool,
                                               .descriptorSetCount = m_framesInFlight,
                                               .pSetLayouts = layouts.data()};
    std::vector<vk::DescriptorSet> rawSets;
    rawSets.reserve(m_framesInFlight);
    auto allocatedSets = m_context.device().allocateDescriptorSets(allocateInfo);
    CHECK_EQ(allocatedSets.size(), static_cast<size_t>(m_framesInFlight));
    for (auto& allocatedSet : allocatedSets) {
      rawSets.push_back(allocatedSet.release());
    }

    slots.reserve(m_framesInFlight);
    for (uint32_t slotIndex = 0u; slotIndex < m_framesInFlight; ++slotIndex) {
      auto slot = std::make_unique<BindlessSlotState>();
      CHECK_LE(sampledImagesPerSet, std::numeric_limits<size_t>::max());
      slot->pendingRetirements.reserve(static_cast<size_t>(sampledImagesPerSet));
      slot->pendingRetirementResources.reserve(static_cast<size_t>(sampledImagesPerSet));
      const vk::DescriptorSet raw = rawSets[slotIndex];
      CHECK(raw != vk::DescriptorSet{}) << "Failed to allocate device bindless descriptor set";
      slot->descriptorSet = std::make_unique<ZVulkanBindlessDescriptorSet>(*this,
                                                                           raw,
                                                                           caps.texture2D,
                                                                           caps.texture2DArray,
                                                                           caps.texture3D,
                                                                           caps.uTexture2D,
                                                                           caps.uTexture3D);

      const std::array<std::pair<ZVulkanBindlessDescriptorSet::Kind, ZVulkanTexture*>, 5> placeholders{
        std::pair{ZVulkanBindlessDescriptorSet::Kind::Texture2D,      m_bindlessPlaceholder2D.get()     },
        std::pair{ZVulkanBindlessDescriptorSet::Kind::Texture2DArray, m_bindlessPlaceholder2DArray.get()},
        std::pair{ZVulkanBindlessDescriptorSet::Kind::Texture3D,      m_bindlessPlaceholder3D.get()     },
        std::pair{ZVulkanBindlessDescriptorSet::Kind::UTexture2D,     m_bindlessPlaceholderU2D.get()    },
        std::pair{ZVulkanBindlessDescriptorSet::Kind::UTexture3D,     m_bindlessPlaceholderU3D.get()    },
      };
      for (const auto& [kind, texture] : placeholders) {
        ZVulkanBindlessDescriptorSet::RegisterRequest request{};
        request.kind = kind;
        request.texture = texture;
        request.debugLabel = "device_bindless_placeholder";
        const uint32_t index = slot->descriptorSet->registerTexture(request);
        CHECK_EQ(index, 0u) << "Device bindless placeholder must reserve index zero for every table";
      }
      slots.emplace_back(std::move(slot));
    }
  }
  catch (...) {
    descriptorPool.reset();
    if (updateAfterBindReservation != 0u) {
      releaseUpdateAfterBindDescriptors(updateAfterBindReservation);
    }
    throw;
  }
  m_bindlessDescriptorPool = std::move(descriptorPool);
  m_bindlessPoolUpdateAfterBindReservation = updateAfterBindReservation;
  m_bindlessSlots = std::move(slots);

  const auto& support = m_context.selectedDeviceSupport();
  if (m_context.supportsDescriptorIndexingSampledImageUpdateAfterBind()) {
    CHECK_EQ(updateAfterBindDescriptorsReserved(), support.requiredUpdateAfterBindDescriptors)
      << "Device bindless pools did not reserve the evaluated update-after-bind descriptor count";
  } else {
    CHECK_EQ(updateAfterBindDescriptorsReserved(), 0u);
  }
}

vk::DescriptorSetLayout ZVulkanDevice::bindlessSampledImageDescriptorSetLayout()
{
  checkOwnerThread("access bindless descriptor-set layout");
  ensureBindlessDescriptorSetLayout();
  CHECK(m_bindlessDescriptorSetLayout.has_value());
  return **m_bindlessDescriptorSetLayout;
}

ZVulkanBindlessDescriptorSet& ZVulkanDevice::bindlessSampledImagesForFrameSlot(uint32_t frameSlot)
{
  checkOwnerThread("access bindless frame-slot table");
  CHECK_EQ(m_bindlessSlots.size(), static_cast<size_t>(m_framesInFlight))
    << "Bindless device state must be prepared before acquiring a frame slot";
  CHECK_LT(frameSlot, m_framesInFlight) << "Bindless frame-slot index out of range";
  CHECK(m_bindlessSlots[frameSlot] != nullptr);
  CHECK(m_bindlessSlots[frameSlot]->descriptorSet != nullptr);
  return *m_bindlessSlots[frameSlot]->descriptorSet;
}

ZVulkanBindlessDescriptorSet& ZVulkanDevice::bindlessSampledImagesForFrame(ZVulkanFrameExecutor::ActiveFrame& frame)
{
  checkOwnerThread("access mutable bindless frame table");
  CHECK(frame.valid()) << "Bindless descriptor table requested with an invalid executor frame";
  CHECK(frameExecutor().isPreRecordSafePoint(frame))
    << "Mutable bindless descriptor table requested outside its pre-record safe point";
  const uint32_t slot = frame.slotIndex();
  CHECK_LT(slot, m_bindlessSlots.size());
  CHECK(m_bindlessSlots[slot] != nullptr);
  CHECK_EQ(m_bindlessSlots[slot]->registrationSerial, frame.acquisitionSerial())
    << "Bindless descriptor table requested before beginning this frame-slot epoch";
  return bindlessSampledImagesForFrameSlot(slot);
}

ZVulkanTexture& ZVulkanDevice::defaultBindlessPlaceholderTexture2D()
{
  checkOwnerThread("access default bindless placeholder");
  ensureBindlessPlaceholderTextures();
  CHECK(m_bindlessPlaceholder2D != nullptr);
  return *m_bindlessPlaceholder2D;
}

bool ZVulkanDevice::retireBindlessTexture(const ZVulkanTexture& texture, const std::shared_ptr<void>& deferredResources)
{
  checkOwnerThread("retire bindless texture");
  const uint64_t identity = texture.descriptorIdentity();
  CHECK_NE(identity, 0u) << "Retiring a Vulkan texture without a descriptor identity";
  CHECK(deferredResources != nullptr) << "Bindless texture retirement requires a GPU-resource holder";

  constexpr std::array kinds{ZVulkanBindlessDescriptorSet::Kind::Texture2D,
                             ZVulkanBindlessDescriptorSet::Kind::Texture2DArray,
                             ZVulkanBindlessDescriptorSet::Kind::Texture3D,
                             ZVulkanBindlessDescriptorSet::Kind::UTexture2D,
                             ZVulkanBindlessDescriptorSet::Kind::UTexture3D};
  bool queued = false;
  for (auto& slot : m_bindlessSlots) {
    CHECK(slot != nullptr);
    CHECK(slot->descriptorSet != nullptr);
    bool queuedForSlot = false;
    for (const auto kind : kinds) {
      const auto handle = slot->descriptorSet->retirementHandle(kind, &texture, identity);
      if (!handle.has_value()) {
        continue;
      }
      CHECK_LT(slot->pendingRetirements.size(), slot->pendingRetirements.capacity())
        << "Bindless retirement queue exceeded the slot's descriptor-backed capacity";
      slot->pendingRetirements.push_back(*handle);
      queuedForSlot = true;
      queued = true;
    }
    if (queuedForSlot) {
      CHECK_LT(slot->pendingRetirementResources.size(), slot->pendingRetirementResources.capacity());
      slot->pendingRetirementResources.push_back(deferredResources);
    }
  }
  return queued;
}

void ZVulkanDevice::beginBindlessFrameSlot(ZVulkanFrameExecutor::ActiveFrame& frame)
{
  checkOwnerThread("begin bindless frame slot");
  CHECK(frame.valid()) << "Beginning bindless frame-slot state with an invalid executor frame";
  CHECK(frameExecutor().owns(frame)) << "Bindless frame slot belongs to another Vulkan device";
  CHECK(frameExecutor().isPreRecordSafePoint(frame))
    << "Beginning bindless frame-slot state outside its pre-record safe point";
  const uint32_t slot = frame.slotIndex();
  CHECK_LT(slot, m_bindlessSlots.size());
  CHECK(m_bindlessSlots[slot] != nullptr);
  CHECK_NE(m_bindlessSlots[slot]->registrationSerial, frame.acquisitionSerial())
    << "Bindless frame-slot epoch begun twice for one frame acquisition";
  applyBindlessRetirementsForFrameSlot(slot);
  bindlessSampledImagesForFrameSlot(slot).beginRegistrationEpoch();
  m_bindlessSlots[slot]->registrationSerial = frame.acquisitionSerial();
}

void ZVulkanDevice::drainBindlessRetirements(ZVulkanFrameExecutor::ActiveFrame& frame)
{
  checkOwnerThread("drain bindless frame-slot retirements");
  CHECK(frame.valid()) << "Draining bindless retirements with an invalid executor frame";
  CHECK(frameExecutor().owns(frame)) << "Bindless retirement frame belongs to another Vulkan device";
  CHECK(frameExecutor().isPreRecordSafePoint(frame))
    << "Draining bindless retirements outside the frame's pre-record safe point";
  applyBindlessRetirementsForFrameSlot(frame.slotIndex());
}

bool ZVulkanDevice::waitForAllFramesAndDrainBindlessRetirements()
{
  checkOwnerThread("wait and drain all bindless retirements");
  if (m_frameExecutor != nullptr) {
    m_frameExecutor->waitForAllInFlight();
  }
  if (!descriptorSetWritesAllowed() ||
      (m_frameExecutor != nullptr && !m_frameExecutor->allFrameSlotsDescriptorMutationSafe())) {
    // A current, unsubmitted command buffer is outside waitForAllInFlight()'s
    // fence domain and may already reference the slot's descriptor set. Keep
    // the deferred holders alive; the normal post-record slot safe point will
    // consume them. Strict-budget recovery will report pressure if this leaves
    // the requested allocation unsatisfied.
    VLOG(1) << "VK bindless retirement: all-slot drain deferred because a command buffer is recording";
    return false;
  }
  // Every executor slot is now fence-safe. Release all deferred texture
  // resources, including those whose allocations contributed to the memory
  // pressure that triggered this strong-recovery path.
  for (uint32_t slot = 0u; slot < m_bindlessSlots.size(); ++slot) {
    applyBindlessRetirementsForFrameSlot(slot);
  }
  return true;
}

void ZVulkanDevice::applyBindlessRetirementsForFrameSlot(uint32_t frameSlot)
{
  checkOwnerThread("apply bindless frame-slot retirements");
  CHECK(descriptorSetWritesAllowed()) << "Applying bindless retirements while a backend is recording";
  CHECK_LT(frameSlot, m_bindlessSlots.size()) << "Bindless retirement frame-slot index out of range";
  auto& slotState = m_bindlessSlots[frameSlot];
  CHECK(slotState != nullptr);
  CHECK(slotState->descriptorSet != nullptr);
  if (slotState->pendingRetirements.empty()) {
    CHECK(slotState->pendingRetirementResources.empty());
    return;
  }
  CHECK(!slotState->pendingRetirementResources.empty());

  CHECK(m_bindlessPlaceholder2D != nullptr);
  CHECK(m_bindlessPlaceholder2DArray != nullptr);
  CHECK(m_bindlessPlaceholder3D != nullptr);
  CHECK(m_bindlessPlaceholderU2D != nullptr);
  CHECK(m_bindlessPlaceholderU3D != nullptr);
  ZVulkanBindlessDescriptorSet::PlaceholderDescriptorInfos placeholderInfos{
    m_bindlessPlaceholder2D->descriptorInfo(),
    m_bindlessPlaceholder2DArray->descriptorInfo(),
    m_bindlessPlaceholder3D->descriptorInfo(),
    m_bindlessPlaceholderU2D->descriptorInfo(),
    m_bindlessPlaceholderU3D->descriptorInfo(),
  };
  for (auto& info : placeholderInfos) {
    info.sampler = vk::Sampler{};
    CHECK(info.imageView != vk::ImageView{});
  }
  for (const auto& holder : slotState->pendingRetirementResources) {
    CHECK(holder != nullptr) << "Bindless retirement lost its deferred GPU-resource holder";
  }
  slotState->descriptorSet->retireEntries(slotState->pendingRetirements, placeholderInfos);
  slotState->pendingRetirements.clear();
  // Release GPU resources only after every descriptor has been sanitized and
  // its table index is no longer reachable.
  slotState->pendingRetirementResources.clear();
}

std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createTransientDescriptorPool()
{
  checkOwnerThread("create transient descriptor pool");
  return std::make_unique<ZVulkanDescriptorPool>(*this, ZVulkanDescriptorPoolKind::Transient);
}

std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createPersistentDescriptorPool()
{
  checkOwnerThread("create persistent descriptor pool");
  return std::make_unique<ZVulkanDescriptorPool>(*this, ZVulkanDescriptorPoolKind::Persistent);
}

std::unique_ptr<ZVulkanDescriptorSet> ZVulkanDevice::createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                                         vk::DescriptorSetLayout layout)
{
  checkOwnerThread("create descriptor set");
  auto descriptorSet = pool.allocateDescriptorSet(layout);
  return std::make_unique<ZVulkanDescriptorSet>(*this, descriptorSet);
}

std::unique_ptr<ZVulkanBuffer> ZVulkanDevice::createBufferInPool(size_t size,
                                                                 vk::BufferUsageFlags usage,
                                                                 vk::MemoryPropertyFlags properties,
                                                                 VmaPool poolOverride)
{
  const auto requestClass = classifyBufferAllocation(usage, properties, poolOverride, *this);
  const bool enforceBudget = countsAgainstDeviceLocalBudget(properties);
  brokerReclaimBeforeAllocation(*this, requestClass, size, enforceBudget, false, "pooled_buffer_preallocate");
  try {
    auto buffer = std::make_unique<ZVulkanBuffer>(*this, size, usage, properties, poolOverride);
    enforceStrictBudgetAfterAllocation(*this, requestClass, size, enforceBudget, "pooled_buffer_postallocate");
    return buffer;
  }
  catch (const std::exception&) {
    brokerReclaimBeforeAllocation(*this, requestClass, size, enforceBudget, true, "pooled_buffer_allocation_retry");
    try {
      auto buffer = std::make_unique<ZVulkanBuffer>(*this, size, usage, properties, poolOverride);
      enforceStrictBudgetAfterAllocation(*this, requestClass, size, enforceBudget, "pooled_buffer_retry_postallocate");
      return buffer;
    }
    catch (const std::exception& retryError) {
      logAllocationFailureReport(*this, requestClass, size, "pooled_buffer_allocation_retry", retryError);
      throw;
    }
  }
}

ZVulkanBuffer& ZVulkanDevice::immediateUploadStagingBuffer(size_t size)
{
  CHECK(size > 0u) << "Immediate upload staging buffer requires a non-empty request";
  if (m_immediateUploadStagingBuffer == nullptr || m_immediateUploadStagingBufferSize < size) {
    m_immediateUploadStagingBuffer =
      createBufferInPool(size,
                         vk::BufferUsageFlagBits::eTransferSrc,
                         vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                         m_uploadStagingPool);
    m_immediateUploadStagingBufferSize = size;
  }
  CHECK(m_immediateUploadStagingBuffer != nullptr);
  return *m_immediateUploadStagingBuffer;
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

ZVulkanDevice::DeviceLocalBudget ZVulkanDevice::deviceLocalBudget() const
{
  DeviceLocalBudget out{};
  if (m_allocator == nullptr) {
    return out;
  }

  VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
  vmaGetHeapBudgets(m_allocator, budgets);

  const auto memProps = m_context.physicalDevice().getMemoryProperties();
  uint32_t bestHeap = UINT32_MAX;
  VkDeviceSize bestBudget = 0;
  for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
    const VkMemoryHeap heap = memProps.memoryHeaps[i];
    const bool deviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u;
    if (!deviceLocal) {
      continue;
    }
    if (budgets[i].budget > bestBudget) {
      bestBudget = budgets[i].budget;
      bestHeap = i;
    }
  }
  if (bestHeap == UINT32_MAX) {
    // Fallback: choose the largest heap if no device-local heap is flagged.
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
      if (budgets[i].budget > bestBudget) {
        bestBudget = budgets[i].budget;
        bestHeap = i;
      }
    }
  }
  if (bestHeap == UINT32_MAX) {
    return out;
  }

  out.budgetBytes = static_cast<uint64_t>(budgets[bestHeap].budget);
  out.usageBytes = static_cast<uint64_t>(budgets[bestHeap].usage);
  return out;
}

} // namespace nim
