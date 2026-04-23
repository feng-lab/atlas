#include "zvulkandevice.h"
#include "zvulkanbuffer.h"
#include "zvulkanresidencymanager.h"
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

#include <algorithm>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace nim {

DEFINE_int32(atlas_vk_frames_in_flight, 2, "Max Vulkan frames in flight (debug: set to 1 to serialize submits)");

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
        device.frameExecutor().waitForAllInFlight();
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
      device.frameExecutor().waitForAllInFlight();
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
{
  // Capture once at startup. Atlas assumes this remains stable for the lifetime
  // of the Vulkan device.
  m_framesInFlight = static_cast<uint32_t>(std::max<int32_t>(1, FLAGS_atlas_vk_frames_in_flight));

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
  info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  const VkResult res = vmaCreateAllocator(&info, &m_allocator);
  if (static_cast<vk::Result>(res) != vk::Result::eSuccess) {
    throw ZException("Failed to create VMA allocator");
  }

  // Create tuned VMA pools
  auto createPool = [&](VkMemoryPropertyFlags reqFlags, VkDeviceSize blockSize, std::string_view label) -> VmaPool {
    VmaPoolCreateInfo pci{};
    const uint32_t typeIndex = findMemoryTypeIndex(reqFlags);
    if (typeIndex == UINT32_MAX) {
      LOG(ERROR) << "VMA: no memory type for " << label << " pool flags=" << std::hex << reqFlags;
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
  try {
    auto domains = phys.getCalibrateableTimeDomainsEXT();
    bool supported = !domains.empty();
    std::string domList;
    for (size_t i = 0; i < domains.size(); ++i) {
      domList += vk::to_string(domains[i]);
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
    LOG(INFO) << "VK calibrated timestamps: query not available; timestampPeriod=" << ts << " ns/tick";
  }
}

ZVulkanDevice::~ZVulkanDevice()
{
  if (m_frameExecutor != nullptr) {
    // Drain all fence-gated completions while the residency manager and VMA
    // allocator are still alive. This flushes deferred unpins and any teardown
    // callbacks that would otherwise outlive the allocator.
    m_frameExecutor->waitForAllInFlight();
  }

  // Managed textures owned by the residency manager destroy VkImages via VMA,
  // so they must be released before the allocator/pools are torn down.
  m_residencyManager.reset();
  m_frameExecutor.reset();
  m_immediateUploadStagingBuffer.reset();
  m_immediateUploadStagingBufferSize = 0;

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
  LOG(INFO) << "Destroying Vulkan device";
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
    m_frameExecutor = std::make_unique<ZVulkanFrameExecutor>(*this, m_framesInFlight);
  } else {
    // Ensure nobody mutates the debug gflag at runtime; changing frames-in-flight
    // mid-run invalidates executor slot identities and can strand fence-gated
    // callbacks (residency unpins, retained UBO lifetimes, etc.).
    const uint32_t fif = static_cast<uint32_t>(std::max<int32_t>(1, FLAGS_atlas_vk_frames_in_flight));
    CHECK(fif == m_framesInFlight) << "FLAGS_atlas_vk_frames_in_flight changed at runtime; this is unsupported";
    CHECK(m_frameExecutor->maxFramesInFlight() == m_framesInFlight)
      << "Vulkan frame executor frames-in-flight changed unexpectedly";
  }
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

std::unique_ptr<ZVulkanDescriptorPool> ZVulkanDevice::createDescriptorPool()
{
  return std::make_unique<ZVulkanDescriptorPool>(*this);
}

std::unique_ptr<ZVulkanDescriptorSet> ZVulkanDevice::createDescriptorSet(ZVulkanDescriptorPool& pool,
                                                                         vk::DescriptorSetLayout layout)
{
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
