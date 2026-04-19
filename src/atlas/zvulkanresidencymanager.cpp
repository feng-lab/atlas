#include "zvulkanresidencymanager.h"

#include "z3dimg.h"
#include "zexception.h"
#include "zimg.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"
#include "zvulkantexture.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

DEFINE_uint64(atlas_vk_residency_budget_bytes,
              0,
              "Strict upper bound (bytes) for Atlas-owned device-local Vulkan residency. "
              "0 uses the device-reported budget for broker reclaim only.");

DEFINE_double(atlas_vk_residency_budget_ratio,
              1.0,
              "Multiplier applied to the effective device-local budget for the Vulkan residency broker. "
              "Use <1.0 to make the resulting device-local residency budget strict.");

DEFINE_uint64(atlas_vk_residency_budget_reserve_bytes,
              0,
              "Reserve this many device-local bytes by subtracting them from the strict Vulkan residency budget.");

namespace nim {

namespace {

constexpr uint64_t kResidencyReclaimMinHysteresisBytes = 32ull * 1024ull * 1024ull;
constexpr uint64_t kResidencyReclaimMaxHysteresisBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kResidencyReclaimRequestDivisor = 8ull;
constexpr uint64_t kResidencyReclaimBudgetDivisor = 32ull;

vk::SamplerCreateInfo makeLinearSampler()
{
  vk::SamplerCreateInfo sampler{};
  sampler.magFilter = vk::Filter::eLinear;
  sampler.minFilter = vk::Filter::eLinear;
  sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
  sampler.addressModeU = vk::SamplerAddressMode::eClampToBorder;
  sampler.addressModeV = vk::SamplerAddressMode::eClampToBorder;
  sampler.addressModeW = vk::SamplerAddressMode::eClampToBorder;
  sampler.unnormalizedCoordinates = false;
  sampler.borderColor = vk::BorderColor::eFloatTransparentBlack;
  sampler.compareEnable = false;
  sampler.anisotropyEnable = false;
  sampler.maxAnisotropy = 1.0f;
  sampler.minLod = 0.0f;
  sampler.maxLod = 0.0f;
  return sampler;
}

std::unique_ptr<ZVulkanTexture> createPagedImageCacheTexture(ZVulkanDevice& device, glm::uvec3 size)
{
  if (size.x == 0u || size.y == 0u || size.z == 0u) {
    return nullptr;
  }

  auto info = ZVulkanTexture::CreateInfo::make3D(
    size.x,
    size.y,
    size.z,
    vk::Format::eR8Unorm,
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
    vk::MemoryPropertyFlagBits::eDeviceLocal,
    1u,
    false,
    vk::ImageLayout::eShaderReadOnlyOptimal);
  info.createDefaultSampler = false;
  info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::PagedImageCacheR8;
  info.samplerInfo = makeLinearSampler();
  return std::make_unique<ZVulkanTexture>(device, info);
}

std::unique_ptr<ZVulkanTexture> createDenseVolumeTexture(ZVulkanDevice& device, glm::uvec3 size)
{
  CHECK(size.x > 0u && size.y > 0u && size.z > 0u) << "Dense volume texture size must be non-zero";

  auto info =
    ZVulkanTexture::CreateInfo::make3D(size.x,
                                       size.y,
                                       size.z,
                                       vk::Format::eR8Unorm,
                                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                       1u,
                                       true,
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
  info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::DenseVolumeTexture;
  return std::make_unique<ZVulkanTexture>(device, info);
}

std::unique_ptr<ZVulkanTexture> createDenseImage2DTexture(ZVulkanDevice& device, glm::uvec3 size)
{
  CHECK(size.x > 0u && size.y > 0u && size.z == 1u) << "Dense 2D image texture size must be non-zero 2D";

  auto info =
    ZVulkanTexture::CreateInfo::make2D(size.x,
                                       size.y,
                                       vk::Format::eR8Unorm,
                                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                       1u,
                                       true,
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
  info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::DenseImageTexture;
  return std::make_unique<ZVulkanTexture>(device, info);
}

uint64_t pagedImageCacheByteSizeFor(const ZVulkanTexture& texture)
{
  const auto extent = texture.extent();
  CHECK(texture.format() == vk::Format::eR8Unorm)
    << "Only R8 image caches are supported for Vulkan paging swap; fmt=" << enumOrUnderlying(texture.format(), 16);
  return static_cast<uint64_t>(extent.width) * extent.height * std::max<uint32_t>(1u, extent.depth);
}

uint64_t denseR8VolumeByteSizeFor(glm::uvec3 size)
{
  CHECK(size.x > 0u && size.y > 0u && size.z > 0u) << "Dense volume texture size must be non-zero";
  return static_cast<uint64_t>(size.x) * size.y * std::max<uint32_t>(1u, size.z);
}

uint64_t denseR8ImageByteSizeFor(glm::uvec3 size)
{
  CHECK(size.x > 0u && size.y > 0u && size.z > 0u) << "Dense image texture size must be non-zero";
  return static_cast<uint64_t>(size.x) * size.y * std::max<uint32_t>(1u, size.z);
}

bool isHostBackedDenseTextureKind(ZVulkanResidencyManager::TextureKind kind)
{
  switch (kind) {
    case ZVulkanResidencyManager::TextureKind::DenseVolumeR8:
    case ZVulkanResidencyManager::TextureKind::DenseImage2DR8:
      return true;
    case ZVulkanResidencyManager::TextureKind::PagedImageCacheR8:
      return false;
  }
  return false;
}

bool isManagedTextureResourceClass(ZVulkanResidencyManager::ResourceClass resourceClass)
{
  switch (resourceClass) {
    case ZVulkanResidencyManager::ResourceClass::DenseImageTexture:
    case ZVulkanResidencyManager::ResourceClass::DenseVolumeTexture:
    case ZVulkanResidencyManager::ResourceClass::PagedImageCacheR8:
      return true;
    default:
      return false;
  }
}

const char* textureKindName(ZVulkanResidencyManager::TextureKind kind)
{
  switch (kind) {
    case ZVulkanResidencyManager::TextureKind::PagedImageCacheR8:
      return "paged_image_cache_r8";
    case ZVulkanResidencyManager::TextureKind::DenseVolumeR8:
      return "dense_volume_r8";
    case ZVulkanResidencyManager::TextureKind::DenseImage2DR8:
      return "dense_image_2d_r8";
  }
  return "<unknown>";
}

ZVulkanResidencyManager::ResourceClass resourceClassForTextureKind(ZVulkanResidencyManager::TextureKind kind)
{
  switch (kind) {
    case ZVulkanResidencyManager::TextureKind::PagedImageCacheR8:
      return ZVulkanResidencyManager::ResourceClass::PagedImageCacheR8;
    case ZVulkanResidencyManager::TextureKind::DenseVolumeR8:
      return ZVulkanResidencyManager::ResourceClass::DenseVolumeTexture;
    case ZVulkanResidencyManager::TextureKind::DenseImage2DR8:
      return ZVulkanResidencyManager::ResourceClass::DenseImageTexture;
  }
  CHECK(false) << "Unknown managed texture kind";
  return ZVulkanResidencyManager::ResourceClass::DenseVolumeTexture;
}

uint64_t residentByteEstimateForKind(ZVulkanResidencyManager::TextureKind kind, glm::uvec3 size)
{
  switch (kind) {
    case ZVulkanResidencyManager::TextureKind::DenseVolumeR8:
    case ZVulkanResidencyManager::TextureKind::DenseImage2DR8:
      return denseR8ImageByteSizeFor(size);
    case ZVulkanResidencyManager::TextureKind::PagedImageCacheR8:
      return static_cast<uint64_t>(size.x) * size.y * std::max<uint32_t>(1u, size.z);
  }
  CHECK(false) << "Unknown managed texture kind";
  return 0u;
}

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs)
{
  if (lhs > std::numeric_limits<uint64_t>::max() - rhs) {
    return std::numeric_limits<uint64_t>::max();
  }
  return lhs + rhs;
}

uint64_t applyResidencyBudgetFlags(uint64_t budget)
{
  if (FLAGS_atlas_vk_residency_budget_bytes > 0) {
    budget = budget == 0 ? FLAGS_atlas_vk_residency_budget_bytes
                         : std::min<uint64_t>(budget, FLAGS_atlas_vk_residency_budget_bytes);
  }

  if (budget == 0) {
    return 0;
  }

  const double ratioRaw = FLAGS_atlas_vk_residency_budget_ratio;
  const double ratio = std::isfinite(ratioRaw) ? std::clamp(ratioRaw, 0.0, 1.0) : 1.0;
  budget = static_cast<uint64_t>(static_cast<double>(budget) * ratio);

  const uint64_t reserve = FLAGS_atlas_vk_residency_budget_reserve_bytes;
  if (reserve >= budget) {
    return 0;
  }
  return budget - reserve;
}

bool strictResidencyBudgetActive()
{
  const double ratioRaw = FLAGS_atlas_vk_residency_budget_ratio;
  const bool reducedRatio = std::isfinite(ratioRaw) && ratioRaw < 1.0;
  return FLAGS_atlas_vk_residency_budget_bytes > 0u || FLAGS_atlas_vk_residency_budget_reserve_bytes > 0u ||
         reducedRatio;
}

uint64_t reclaimHysteresisBytes(uint64_t allocationBytes, uint64_t effectiveBudget)
{
  if (allocationBytes == 0u || effectiveBudget == 0u) {
    return 0u;
  }

  const uint64_t requestScaled =
    std::max<uint64_t>(allocationBytes / kResidencyReclaimRequestDivisor, kResidencyReclaimMinHysteresisBytes);
  const uint64_t budgetScaled = effectiveBudget / kResidencyReclaimBudgetDivisor;
  const uint64_t capped = std::min<uint64_t>(requestScaled, kResidencyReclaimMaxHysteresisBytes);
  return budgetScaled == 0u ? capped : std::min<uint64_t>(capped, budgetScaled);
}

ZVulkanResidencyManager::AllocationPressure
makeAllocationPressure(uint64_t usageBytes, uint64_t budgetBytes, uint64_t allocationBytes, bool strictBudget)
{
  ZVulkanResidencyManager::AllocationPressure pressure{};
  pressure.usageBytes = usageBytes;
  pressure.budgetBytes = budgetBytes;
  pressure.allocationBytes = allocationBytes;

  if (budgetBytes == 0u) {
    if (strictBudget && (usageBytes > 0u || allocationBytes > 0u)) {
      pressure.reclaimBytes = saturatingAdd(usageBytes, allocationBytes);
    }
    return pressure;
  }

  if (strictBudget && usageBytes > budgetBytes) {
    pressure.reclaimBytes = usageBytes - budgetBytes;
    return pressure;
  }

  const uint64_t projectedUsage = saturatingAdd(usageBytes, allocationBytes);
  if (projectedUsage <= budgetBytes) {
    return pressure;
  }

  const uint64_t overageBytes = projectedUsage - budgetBytes;
  pressure.reclaimBytes =
    strictBudget ? overageBytes : saturatingAdd(overageBytes, reclaimHysteresisBytes(allocationBytes, budgetBytes));
  return pressure;
}

class DeviceAllocationRecoveryScope final
{
public:
  explicit DeviceAllocationRecoveryScope(ZVulkanDevice& device)
    : m_device(device)
  {
    m_device.enterAllocationRecoveryScope();
  }

  DeviceAllocationRecoveryScope(const DeviceAllocationRecoveryScope&) = delete;
  DeviceAllocationRecoveryScope& operator=(const DeviceAllocationRecoveryScope&) = delete;

  ~DeviceAllocationRecoveryScope()
  {
    m_device.leaveAllocationRecoveryScope();
  }

private:
  ZVulkanDevice& m_device;
};

void clearPagedImageCacheToZero(ZVulkanDevice& device, ZVulkanTexture& texture)
{
  CHECK(texture.resident()) << "clearPagedImageCacheToZero requires a resident texture";
  CHECK(texture.format() == vk::Format::eR8Unorm)
    << "clearPagedImageCacheToZero expected R8 image cache; fmt=" << enumOrUnderlying(texture.format(), 16);

  device.frameExecutor().executeImmediate(
    [&](vk::raii::CommandBuffer& cmd) {
      const vk::ImageLayout startLayout = texture.layout();
      texture.transitionLayout(cmd, startLayout, vk::ImageLayout::eTransferDstOptimal, vk::ImageAspectFlagBits::eColor);
      vk::ClearColorValue zero{
        std::array<float, 4>{0.f, 0.f, 0.f, 0.f}
      };
      vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,
                                      0u,
                                      texture.mipLevels(),
                                      0u,
                                      texture.arrayLayers()};
      cmd.clearColorImage(texture.image(), vk::ImageLayout::eTransferDstOptimal, zero, range);
      texture.transitionLayout(cmd,
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageLayout::eShaderReadOnlyOptimal,
                               vk::ImageAspectFlagBits::eColor);
    },
    "residency_cache_clear");
  texture.setDescriptorLayout(vk::ImageLayout::eShaderReadOnlyOptimal);
}

} // namespace

ZVulkanResidencyManager::ZVulkanResidencyManager(ZVulkanDevice& device)
  : m_device(device)
  , m_aliveFlag(std::make_shared<std::atomic_bool>(true))
{}

ZVulkanResidencyManager::~ZVulkanResidencyManager()
{
  m_aliveFlag->store(false, std::memory_order_relaxed);
}

ZVulkanResidencyManager::TextureProtectionScope::TextureProtectionScope(ZVulkanResidencyManager* manager,
                                                                        std::vector<ZVulkanTexture*> textures)
  : m_manager(manager)
  , m_textures(std::move(textures))
{}

ZVulkanResidencyManager::TextureProtectionScope::~TextureProtectionScope()
{
  release();
}

ZVulkanResidencyManager::TextureProtectionScope::TextureProtectionScope(TextureProtectionScope&& other) noexcept
  : m_manager(std::exchange(other.m_manager, nullptr))
  , m_textures(std::move(other.m_textures))
{}

ZVulkanResidencyManager::TextureProtectionScope&
ZVulkanResidencyManager::TextureProtectionScope::operator=(TextureProtectionScope&& other) noexcept
{
  if (this == &other) {
    return *this;
  }
  release();
  m_manager = std::exchange(other.m_manager, nullptr);
  m_textures = std::move(other.m_textures);
  return *this;
}

void ZVulkanResidencyManager::TextureProtectionScope::release()
{
  if (m_manager == nullptr) {
    return;
  }
  m_manager->releaseTextureProtections(std::span<ZVulkanTexture* const>(m_textures.data(), m_textures.size()));
  m_manager = nullptr;
  m_textures.clear();
}

size_t ZVulkanResidencyManager::TextureKeyHash::operator()(const TextureKey& key) const noexcept
{
  // Pointer identity is sufficient (owners are long-lived objects).
  const auto p = reinterpret_cast<uintptr_t>(key.owner);
  const auto k = static_cast<uintptr_t>(key.kind);
  const auto i = static_cast<uintptr_t>(key.index);
  // Very simple mix; good enough for pointer-heavy keys.
  return static_cast<size_t>(p ^ (k << 1u) ^ (i << 17u));
}

ZVulkanResidencyManager::ManagedTexture& ZVulkanResidencyManager::ensureRecordLocked(const TextureKey& key)
{
  auto it = m_textures.find(key);
  if (it != m_textures.end()) {
    return it->second;
  }
  ManagedTexture rec{};
  rec.key = key;
  auto [insertedIt, _] = m_textures.emplace(key, std::move(rec));
  return insertedIt->second;
}

void ZVulkanResidencyManager::resetRecordForSizeLocked(ManagedTexture& record, glm::uvec3 cacheSize)
{
  if (record.logicalSize == cacheSize) {
    return;
  }

  CHECK(record.pinCount == 0u) << "Attempted to resize managed texture while pinned (in-flight use)";

  if (record.texture) {
    m_reverse.erase(record.texture.get());
    record.texture.reset();
  }
  record.logicalSize = cacheSize;
  record.sourceImage.reset();
  record.sourceGeneration = 0;
  record.dirty = false;
  record.knownZero = true;
  record.hostValid = false;
  record.lastUsedTick = 0;
  record.hostData.clear();
}

uint64_t ZVulkanResidencyManager::effectiveResidencyBudgetBytesLocked() const
{
  const auto dev = m_device.deviceLocalBudget();
  return applyResidencyBudgetFlags(dev.budgetBytes);
}

uint64_t ZVulkanResidencyManager::managedResidentBytesLocked() const
{
  uint64_t bytes = 0u;
  for (const auto& [_, record] : m_textures) {
    if (record.texture && record.texture->resident()) {
      bytes = saturatingAdd(bytes, record.texture->allocationSizeBytes());
    }
  }
  return bytes;
}

ZVulkanResidencyManager::AllocationPressure
ZVulkanResidencyManager::allocationPressureForLocked(uint64_t allocationBytes) const
{
  const auto budget = m_device.deviceLocalBudget();
  const uint64_t effectiveBudget = applyResidencyBudgetFlags(budget.budgetBytes);
  const uint64_t usageBytes = strictResidencyBudgetActive() ? managedResidentBytesLocked() : budget.usageBytes;
  return makeAllocationPressure(usageBytes, effectiveBudget, allocationBytes, strictResidencyBudgetActive());
}

ZVulkanResidencyManager::AllocationPressure
ZVulkanResidencyManager::allocationPressureFor(uint64_t allocationBytes) const
{
  if (strictResidencyBudgetActive()) {
    const uint64_t budgetBytes = effectiveBrokerBudgetBytes();
    uint64_t residentBytes = 0u;
    for (const ResourceReport& report : memoryReports()) {
      if (!resourceClassCountsAgainstStrictBudget(report.resourceClass)) {
        continue;
      }
      residentBytes = saturatingAdd(residentBytes, report.residentBytes);
    }
    return makeAllocationPressure(residentBytes, budgetBytes, allocationBytes, true);
  }

  std::scoped_lock lock(m_mutex);
  return allocationPressureForLocked(allocationBytes);
}

void ZVulkanResidencyManager::reclaimBeforeManagedTextureAllocation(ResourceClass requestClass,
                                                                    uint64_t allocationBytes,
                                                                    std::string_view reason)
{
  AllocationPressure pressure = allocationPressureFor(allocationBytes);
  if (!pressure.needsReclaim()) {
    return;
  }

  const AllocationPressure initialPressure = pressure;
  bool waitedForCompletion = false;
  ReclaimStats totalStats{};
  while (pressure.needsReclaim()) {
    const auto reclaimStats = reclaimMemory(ReclaimRequest{.requestClass = requestClass,
                                                           .requestedBytes = pressure.reclaimBytes,
                                                           .force = false,
                                                           .reason = reason});
    totalStats.add(reclaimStats);

    if (!strictBudgetActive()) {
      break;
    }

    const AllocationPressure retryPressure = allocationPressureFor(allocationBytes);
    if (!retryPressure.needsReclaim()) {
      pressure = retryPressure;
      break;
    }

    if (reclaimStats.resourcesReleased == 0u && reclaimStats.bytesReleased == 0u) {
      if (!waitedForCompletion) {
        const uint32_t inFlightBefore = m_device.frameExecutor().inFlightCount();
        VLOG(1) << fmt::format(
          "VK managed allocation broker strong recovery: class={} allocation={}B unreclaimed={}B in_flight={} reason='{}'",
          resourceClassName(requestClass),
          allocationBytes,
          retryPressure.reclaimBytes,
          inFlightBefore,
          reason.empty() ? "<unspecified>" : std::string(reason));
        m_device.frameExecutor().waitForAllInFlight();
        waitedForCompletion = true;
        pressure = allocationPressureFor(allocationBytes);
        if (!pressure.needsReclaim()) {
          break;
        }
        continue;
      }
      throw ZException(fmt::format(
        "VK residency budget exceeded before managed texture allocation: class={} requested={}B usage={}B budget={}B unreclaimed={}B released={}B resources={} reason='{}' residents=[{}]",
        resourceClassName(requestClass),
        allocationBytes,
        retryPressure.usageBytes,
        retryPressure.budgetBytes,
        retryPressure.reclaimBytes,
        totalStats.bytesReleased,
        totalStats.resourcesReleased,
        reason.empty() ? "<unspecified>" : std::string(reason),
        describeMemoryByLabel()));
    }

    pressure = retryPressure;
  }

  if (VLOG_IS_ON(1)) {
    const auto afterPressure = allocationPressureFor(0u);
    VLOG(1) << fmt::format(
      "VK managed allocation broker reclaim: class={} allocation={}B target={}B released={}B resources={} usage_before={}B budget={}B usage_after={}B reason='{}'",
      resourceClassName(requestClass),
      allocationBytes,
      initialPressure.reclaimBytes,
      totalStats.bytesReleased,
      totalStats.resourcesReleased,
      initialPressure.usageBytes,
      initialPressure.budgetBytes,
      afterPressure.usageBytes,
      reason.empty() ? "<unspecified>" : std::string(reason));
  }
}

ZVulkanTexture*
ZVulkanResidencyManager::pagedImageCacheTexture(const void* owner, uint32_t channel, glm::uvec3 cacheSize)
{
  CHECK(owner != nullptr) << "pagedImageCacheTexture called with null owner";
  CHECK(cacheSize.x > 0u && cacheSize.y > 0u && cacheSize.z > 0u) << "Invalid Vulkan imageCacheSize";

  TextureKey key{};
  key.owner = owner;
  key.kind = TextureKind::PagedImageCacheR8;
  key.index = channel;

  bool needsAllocation = false;
  const uint64_t neededBytes = static_cast<uint64_t>(cacheSize.x) * cacheSize.y * std::max<uint32_t>(1u, cacheSize.z);
  {
    std::scoped_lock lock(m_mutex);
    auto& record = ensureRecordLocked(key);
    resetRecordForSizeLocked(record, cacheSize);
    needsAllocation = !record.texture || !record.texture->resident();
  }

  if (needsAllocation) {
    reclaimBeforeManagedTextureAllocation(ResourceClass::PagedImageCacheR8,
                                          neededBytes,
                                          "paged_image_cache_preallocate");
  }

  std::scoped_lock lock(m_mutex);
  auto& record = ensureRecordLocked(key);
  resetRecordForSizeLocked(record, cacheSize);
  ensurePagedImageCacheResidentLocked(key, record, cacheSize);
  return record.texture.get();
}

ZVulkanTexture* ZVulkanResidencyManager::denseVolumeTexture(Z3DImg& owner,
                                                            uint32_t channel,
                                                            std::shared_ptr<const ZImg> image,
                                                            uint64_t generation)
{
  CHECK(image != nullptr) << "denseVolumeTexture called with null image";
  CHECK(image->info().bytesPerVoxel == 1u) << "Dense Vulkan volume residency currently expects R8 source data";
  CHECK(image->width() > 0 && image->height() > 0 && image->depth() > 0) << "Dense Vulkan volume has empty extent";
  CHECK(image->numChannels() == 1u) << "Dense Vulkan volume source must be a single-channel ZImg view";
  const glm::uvec3 volumeSize{static_cast<uint32_t>(image->width()),
                              static_cast<uint32_t>(image->height()),
                              static_cast<uint32_t>(image->depth())};
  const uint64_t neededBytes = denseR8VolumeByteSizeFor(volumeSize);

  TextureKey key{};
  key.owner = &owner;
  key.kind = TextureKind::DenseVolumeR8;
  key.index = channel;

  bool needsAllocation = false;
  {
    std::scoped_lock lock(m_mutex);
    registerImageOwnerDestructionCallbackLocked(owner);
    auto& record = ensureRecordLocked(key);
    needsAllocation = !record.texture || !record.texture->resident() || record.logicalSize != volumeSize;
  }

  if (needsAllocation) {
    reclaimBeforeManagedTextureAllocation(ResourceClass::DenseVolumeTexture, neededBytes, "dense_volume_preallocate");
  }

  std::scoped_lock lock(m_mutex);
  registerImageOwnerDestructionCallbackLocked(owner);
  auto& record = ensureRecordLocked(key);
  ensureDenseImageResidentLocked(key, record, std::move(image), generation);
  return record.texture.get();
}

ZVulkanTexture* ZVulkanResidencyManager::denseImage2DTexture(Z3DImg& owner,
                                                             uint32_t channel,
                                                             std::shared_ptr<const ZImg> image,
                                                             uint64_t generation)
{
  CHECK(image != nullptr) << "denseImage2DTexture called with null image";
  CHECK(image->info().bytesPerVoxel == 1u) << "Dense Vulkan 2D image residency currently expects R8 source data";
  CHECK(image->width() > 0 && image->height() > 0 && image->depth() == 1) << "Dense Vulkan 2D image has invalid extent";
  CHECK(image->numChannels() == 1u) << "Dense Vulkan 2D image source must be a single-channel ZImg view";
  const glm::uvec3 imageSize{static_cast<uint32_t>(image->width()), static_cast<uint32_t>(image->height()), 1u};
  const uint64_t neededBytes = denseR8ImageByteSizeFor(imageSize);

  TextureKey key{};
  key.owner = &owner;
  key.kind = TextureKind::DenseImage2DR8;
  key.index = channel;

  bool needsAllocation = false;
  {
    std::scoped_lock lock(m_mutex);
    registerImageOwnerDestructionCallbackLocked(owner);
    auto& record = ensureRecordLocked(key);
    needsAllocation = !record.texture || !record.texture->resident() || record.logicalSize != imageSize;
  }

  if (needsAllocation) {
    reclaimBeforeManagedTextureAllocation(ResourceClass::DenseImageTexture, neededBytes, "dense_image_2d_preallocate");
  }

  std::scoped_lock lock(m_mutex);
  registerImageOwnerDestructionCallbackLocked(owner);
  auto& record = ensureRecordLocked(key);
  ensureDenseImageResidentLocked(key, record, std::move(image), generation);
  return record.texture.get();
}

void ZVulkanResidencyManager::notifyPagedImageCacheWritten(const void* owner, uint32_t channel)
{
  CHECK(owner != nullptr) << "notifyPagedImageCacheWritten called with null owner";
  TextureKey key{};
  key.owner = owner;
  key.kind = TextureKind::PagedImageCacheR8;
  key.index = channel;

  std::scoped_lock lock(m_mutex);
  auto it = m_textures.find(key);
  if (it == m_textures.end()) {
    // Be strict: writes should only occur to managed caches.
    CHECK(false) << "notifyPagedImageCacheWritten called for unknown texture (owner/channel not managed)";
    return;
  }
  auto& record = it->second;
  record.dirty = true;
  record.knownZero = false;
  record.hostValid = false;
  record.lastUsedTick = m_usageTick++;
}

bool ZVulkanResidencyManager::pinIfManaged(ZVulkanTexture* texture)
{
  if (texture == nullptr) {
    return false;
  }
  std::scoped_lock lock(m_mutex);
  auto it = m_reverse.find(texture);
  if (it == m_reverse.end()) {
    return false;
  }
  auto recIt = m_textures.find(it->second);
  CHECK(recIt != m_textures.end());
  recIt->second.pinCount++;
  return true;
}

void ZVulkanResidencyManager::unpinIfManaged(ZVulkanTexture* texture)
{
  if (texture == nullptr) {
    return;
  }
  std::scoped_lock lock(m_mutex);
  auto it = m_reverse.find(texture);
  if (it == m_reverse.end()) {
    return;
  }
  auto recIt = m_textures.find(it->second);
  CHECK(recIt != m_textures.end());
  CHECK(recIt->second.pinCount > 0u) << "Residency unpin underflow";
  recIt->second.pinCount--;
}

ZVulkanResidencyManager::TextureProtectionScope
ZVulkanResidencyManager::protectTextures(std::span<ZVulkanTexture* const> textures)
{
  std::vector<ZVulkanTexture*> protectedTextures;
  if (textures.empty()) {
    return TextureProtectionScope{};
  }

  std::unordered_set<ZVulkanTexture*> seen;
  seen.reserve(textures.size());
  std::scoped_lock lock(m_mutex);
  protectedTextures.reserve(textures.size());
  for (auto* texture : textures) {
    if (texture == nullptr || !seen.insert(texture).second) {
      continue;
    }
    if (m_reverse.find(texture) == m_reverse.end()) {
      continue;
    }
    m_protectedTextures[texture]++;
    protectedTextures.push_back(texture);
  }

  if (protectedTextures.empty()) {
    return TextureProtectionScope{};
  }
  return TextureProtectionScope(this, std::move(protectedTextures));
}

bool ZVulkanResidencyManager::ensureResidentIfManaged(ZVulkanTexture* texture, std::string_view reason)
{
  if (texture == nullptr) {
    return false;
  }

  TextureKey key{};
  ResourceClass resourceClass = ResourceClass::PersistentCompositorTarget;
  uint64_t neededBytes = 0u;
  {
    std::scoped_lock lock(m_mutex);
    auto reverseIt = m_reverse.find(texture);
    if (reverseIt == m_reverse.end()) {
      return false;
    }
    key = reverseIt->second;
    auto recIt = m_textures.find(key);
    CHECK(recIt != m_textures.end()) << "Managed texture reverse map points at missing record";
    ManagedTexture& record = recIt->second;
    CHECK(record.texture.get() == texture) << "Managed texture reverse map points at stale texture";
    if (record.texture->resident()) {
      record.lastUsedTick = m_usageTick++;
      return true;
    }
    CHECK(record.logicalSize.x > 0u && record.logicalSize.y > 0u && record.logicalSize.z > 0u)
      << "Managed texture has no logical size for residency restore";
    resourceClass = resourceClassForTextureKind(key.kind);
    neededBytes = residentByteEstimateForKind(key.kind, record.logicalSize);
  }

  reclaimBeforeManagedTextureAllocation(resourceClass,
                                        neededBytes,
                                        reason.empty() ? std::string_view("managed_texture_hotset") : reason);

  std::scoped_lock lock(m_mutex);
  auto recIt = m_textures.find(key);
  CHECK(recIt != m_textures.end()) << "Managed texture record disappeared during residency restore";
  ManagedTexture& record = recIt->second;
  CHECK(record.texture.get() == texture) << "Managed texture pointer changed during residency restore";
  if (record.texture->resident()) {
    record.lastUsedTick = m_usageTick++;
    return true;
  }

  if (isHostBackedDenseTextureKind(key.kind)) {
    if (!record.sourceImage) {
      throw ZException(
        fmt::format("VK managed dense image restore source unavailable: kind={} owner={} idx={} reason='{}'",
                    textureKindName(key.kind),
                    key.owner,
                    key.index,
                    reason.empty() ? "<unspecified>" : std::string(reason)));
    }
    ensureDenseImageResidentLocked(key, record, record.sourceImage, record.sourceGeneration);
    return true;
  }

  CHECK(key.kind == TextureKind::PagedImageCacheR8)
    << "Unknown managed texture kind during residency restore: " << textureKindName(key.kind);
  ensurePagedImageCacheResidentLocked(key, record, record.logicalSize);
  return true;
}

std::string_view ZVulkanResidencyManager::resourceClassName(ResourceClass resourceClass)
{
  switch (resourceClass) {
    case ResourceClass::TransientUploadPage:
      return "transient_upload_page";
    case ResourceClass::ScratchBacking:
      return "scratch_backing";
    case ResourceClass::StaticGeometry:
      return "static_geometry";
    case ResourceClass::PagedImageMetadataTexture:
      return "paged_image_metadata_texture";
    case ResourceClass::DenseImageTexture:
      return "dense_image_texture";
    case ResourceClass::DenseVolumeTexture:
      return "dense_volume_texture";
    case ResourceClass::PagedImageCacheR8:
      return "paged_image_cache_r8";
    case ResourceClass::ReadbackStaging:
      return "readback_staging";
    case ResourceClass::PersistentCompositorTarget:
      return "persistent_compositor_target";
  }
  return "<unknown>";
}

bool ZVulkanResidencyManager::resourceClassCountsAgainstStrictBudget(ResourceClass resourceClass)
{
  switch (resourceClass) {
    case ResourceClass::TransientUploadPage:
    case ResourceClass::ReadbackStaging:
      return false;
    case ResourceClass::ScratchBacking:
    case ResourceClass::StaticGeometry:
    case ResourceClass::PagedImageMetadataTexture:
    case ResourceClass::DenseImageTexture:
    case ResourceClass::DenseVolumeTexture:
    case ResourceClass::PagedImageCacheR8:
    case ResourceClass::PersistentCompositorTarget:
      return true;
  }
  return true;
}

ZVulkanResidencyManager::ReclaimStats ZVulkanResidencyManager::reclaimMemory(const ReclaimRequest& request)
{
  CHECK(request.scope == ReclaimScope::RequestedClassOnly || request.requestedBytes > 0u || request.force)
    << "Pressure-ladder Vulkan reclaim requires a byte target unless force=true";
  DeviceAllocationRecoveryScope allocationRecoveryScope(m_device);

  ReclaimStats total{};
  const ResourceClass ladder[] = {
    ResourceClass::TransientUploadPage,
    ResourceClass::ScratchBacking,
    ResourceClass::ReadbackStaging,
    ResourceClass::StaticGeometry,
    ResourceClass::DenseImageTexture,
    ResourceClass::PagedImageCacheR8,
    ResourceClass::DenseVolumeTexture,
    ResourceClass::PagedImageMetadataTexture,
    ResourceClass::PersistentCompositorTarget,
  };

  auto targetSatisfied = [&]() {
    return request.requestedBytes > 0u && total.bytesReleased >= request.requestedBytes;
  };

  auto remainingTargetBytes = [&]() -> uint64_t {
    if (request.requestedBytes == 0u) {
      return 0u;
    }
    return total.bytesReleased >= request.requestedBytes ? 0u : request.requestedBytes - total.bytesReleased;
  };

  struct ManagedCandidate
  {
    TextureKey key{};
    EvictionCandidate candidate{};
  };

  struct ProviderCandidate
  {
    ResourceProvider provider;
    EvictionCandidate candidate{};
  };

  auto collectProviders = [&]() {
    std::vector<ResourceProvider> providers;
    std::scoped_lock lock(m_mutex);
    providers.reserve(m_resourceProviders.size());
    for (const auto& [_, record] : m_resourceProviders) {
      providers.push_back(record.provider);
    }
    return providers;
  };

  auto collectManagedCandidates = [&](ResourceClass resourceClass) {
    std::vector<ManagedCandidate> candidates;
    if (!isManagedTextureResourceClass(resourceClass)) {
      return candidates;
    }
    std::scoped_lock lock(m_mutex);
    for (const auto& [key, rec] : m_textures) {
      if (!rec.texture || !rec.texture->resident()) {
        continue;
      }
      const ResourceClass recClass = resourceClassForTextureKind(key.kind);
      if (recClass != resourceClass) {
        continue;
      }
      const uint32_t pinCount = rec.pinCount;
      if (pinCount > 0u || textureProtectedLocked(rec)) {
        continue;
      }
      const bool restoreAvailable = key.kind == TextureKind::PagedImageCacheR8 || rec.sourceImage != nullptr;
      if (!restoreAvailable) {
        continue;
      }
      const uint64_t bytes = rec.texture->allocationSizeBytes();
      if (bytes == 0u) {
        continue;
      }
      candidates.push_back(ManagedCandidate{
        .key = key,
        .candidate = EvictionCandidate{
                                       .resourceClass = recClass,
                                       .priority = 0,
                                       .residentBytes = bytes,
                                       .lastUsedEpoch = rec.lastUsedTick,
                                       .pinCount = pinCount,
                                       .restoreAvailable = restoreAvailable,
                                       .userKey0 = key.index,
                                       .userKey1 = static_cast<uint64_t>(key.kind),
                                       .label = fmt::format("{} owner={} idx={}", textureKindName(key.kind), key.owner, key.index)}
      });
    }
    return candidates;
  };

  auto evictManagedCandidate = [&](const ManagedCandidate& candidate) {
    ReclaimStats stats{};
    std::scoped_lock lock(m_mutex);
    auto it = m_textures.find(candidate.key);
    if (it == m_textures.end()) {
      return stats;
    }
    ManagedTexture& rec = it->second;
    if (!rec.texture || !rec.texture->resident() || rec.pinCount > 0u || textureProtectedLocked(rec)) {
      return stats;
    }
    const uint64_t bytes = rec.texture->allocationSizeBytes();
    evictLocked(rec);
    stats.resourcesReleased = 1;
    stats.bytesReleased = bytes;
    return stats;
  };

  auto candidateLess = [](const EvictionCandidate& a, const EvictionCandidate& b) {
    if (a.priority != b.priority) {
      return a.priority < b.priority;
    }
    if (a.lastUsedEpoch != b.lastUsedEpoch) {
      return a.lastUsedEpoch < b.lastUsedEpoch;
    }
    if (a.residentBytes != b.residentBytes) {
      return a.residentBytes > b.residentBytes;
    }
    return a.label < b.label;
  };

  auto reclaimClassCandidates = [&](ResourceClass resourceClass) {
    while (!targetSatisfied()) {
      std::vector<ManagedCandidate> managedCandidates = collectManagedCandidates(resourceClass);
      std::vector<ProviderCandidate> providerCandidates;
      for (const auto& provider : collectProviders()) {
        if (provider.resourceClass != resourceClass || !provider.collectCandidates || !provider.evictCandidate) {
          continue;
        }
        for (auto candidate : provider.collectCandidates()) {
          if (candidate.resourceClass == ResourceClass::PersistentCompositorTarget) {
            candidate.resourceClass = provider.resourceClass;
          }
          if (candidate.resourceClass != resourceClass || candidate.residentBytes == 0u || candidate.pinCount > 0u ||
              !candidate.restoreAvailable) {
            continue;
          }
          candidate.priority += provider.priority;
          if (candidate.label.empty()) {
            candidate.label = provider.label;
          }
          providerCandidates.push_back(ProviderCandidate{.provider = provider, .candidate = std::move(candidate)});
        }
      }

      const bool haveManaged = !managedCandidates.empty();
      const bool haveProvider = !providerCandidates.empty();
      if (!haveManaged && !haveProvider) {
        break;
      }

      std::sort(managedCandidates.begin(), managedCandidates.end(), [&](const auto& a, const auto& b) {
        return candidateLess(a.candidate, b.candidate);
      });
      std::sort(providerCandidates.begin(), providerCandidates.end(), [&](const auto& a, const auto& b) {
        return candidateLess(a.candidate, b.candidate);
      });

      const EvictionCandidate* managedBest = haveManaged ? &managedCandidates.front().candidate : nullptr;
      const EvictionCandidate* providerBest = haveProvider ? &providerCandidates.front().candidate : nullptr;
      ReclaimStats stats{};
      std::string evictedLabel;
      if (providerBest == nullptr || (managedBest != nullptr && candidateLess(*managedBest, *providerBest))) {
        evictedLabel = managedCandidates.front().candidate.label;
        stats = evictManagedCandidate(managedCandidates.front());
      } else {
        evictedLabel = providerCandidates.front().candidate.label;
        ReclaimRequest candidateRequest = request;
        candidateRequest.requestedBytes = remainingTargetBytes();
        stats =
          providerCandidates.front().provider.evictCandidate(providerCandidates.front().candidate, candidateRequest);
      }

      if (stats.resourcesReleased == 0u && stats.bytesReleased == 0u) {
        break;
      }
      total.add(stats);
      VLOG(1) << fmt::format("VK broker evicted candidate='{}' class={} resources={} bytes={}B reason='{}'",
                             evictedLabel.empty() ? "<unlabeled>" : evictedLabel,
                             resourceClassName(resourceClass),
                             stats.resourcesReleased,
                             stats.bytesReleased,
                             request.reason.empty() ? "<unspecified>" : std::string(request.reason));
    }
  };

  auto reclaimClassProviders = [&](ResourceClass resourceClass) {
    for (const auto& provider : collectProviders()) {
      if (provider.resourceClass != resourceClass || !provider.reclaim) {
        continue;
      }
      ReclaimRequest providerRequest = request;
      providerRequest.requestedBytes = remainingTargetBytes();
      const ReclaimStats stats = provider.reclaim(providerRequest);
      total.add(stats);
      if (stats.resourcesReleased > 0u) {
        VLOG(1) << fmt::format("VK broker reclaimed provider='{}' class={} resources={} bytes={}B reason='{}'",
                               provider.label.empty() ? "<unlabeled>" : provider.label,
                               resourceClassName(resourceClass),
                               stats.resourcesReleased,
                               stats.bytesReleased,
                               request.reason.empty() ? "<unspecified>" : std::string(request.reason));
      }
      if (targetSatisfied()) {
        break;
      }
    }
  };

  auto reclaimClass = [&](ResourceClass resourceClass) {
    reclaimClassCandidates(resourceClass);
    if (!targetSatisfied()) {
      reclaimClassProviders(resourceClass);
    }
    if (!targetSatisfied() && isManagedTextureResourceClass(resourceClass)) {
      std::scoped_lock lock(m_mutex);
      total.add(reclaimManagedTexturesLocked(resourceClass, remainingTargetBytes(), request.reason));
    }
  };

  if (request.scope == ReclaimScope::RequestedClassOnly) {
    reclaimClass(request.requestClass);
  } else {
    for (ResourceClass resourceClass : ladder) {
      reclaimClass(resourceClass);
      if (targetSatisfied()) {
        break;
      }
    }
  }

  if (total.resourcesReleased > 0u) {
    VLOG(1) << fmt::format(
      "VK broker reclaim summary: requested_class={} requested={}B resources={} bytes={}B force={} scope={} reason='{}'",
      resourceClassName(request.requestClass),
      request.requestedBytes,
      total.resourcesReleased,
      total.bytesReleased,
      request.force ? 1 : 0,
      request.scope == ReclaimScope::RequestedClassOnly ? "class_only" : "pressure_ladder",
      request.reason.empty() ? "<unspecified>" : std::string(request.reason));
  } else {
    const bool expectedOptionalStaticMiss = request.requestClass == ResourceClass::StaticGeometry &&
                                            request.scope == ReclaimScope::RequestedClassOnly && !request.force;
    if (expectedOptionalStaticMiss) {
      VLOG(2) << fmt::format(
        "VK broker reclaim found no victims: requested_class={} requested={}B force={} scope={} reason='{}' reports=[{}]",
        resourceClassName(request.requestClass),
        request.requestedBytes,
        request.force ? 1 : 0,
        request.scope == ReclaimScope::RequestedClassOnly ? "class_only" : "pressure_ladder",
        request.reason.empty() ? "<unspecified>" : std::string(request.reason),
        describeMemoryByClass());
    } else if (VLOG_IS_ON(1)) {
      VLOG(1) << fmt::format(
        "VK broker reclaim found no victims: requested_class={} requested={}B force={} scope={} reason='{}' reports=[{}]",
        resourceClassName(request.requestClass),
        request.requestedBytes,
        request.force ? 1 : 0,
        request.scope == ReclaimScope::RequestedClassOnly ? "class_only" : "pressure_ladder",
        request.reason.empty() ? "<unspecified>" : std::string(request.reason),
        describeMemoryByClass());
    }
  }
  return total;
}

ZVulkanResidencyManager::ResourceProviderId ZVulkanResidencyManager::registerResourceProvider(ResourceProvider provider)
{
  CHECK(provider.owner != nullptr) << "Vulkan residency provider requires a stable owner pointer";
  CHECK(provider.reclaim || provider.report || provider.collectCandidates)
    << "Vulkan residency provider must expose reclaim, candidate, or report callback";

  std::scoped_lock lock(m_mutex);
  for (auto& [id, record] : m_resourceProviders) {
    const auto& existing = record.provider;
    if (existing.owner == provider.owner && existing.resourceClass == provider.resourceClass &&
        existing.label == provider.label) {
      CHECK(record.refCount < std::numeric_limits<uint32_t>::max()) << "Vulkan residency provider refcount overflow";
      record.refCount++;
      return id;
    }
  }

  const ResourceProviderId id = m_nextResourceProviderId++;
  const auto [_, inserted] =
    m_resourceProviders.emplace(id, ResourceProviderRecord{.provider = std::move(provider), .refCount = 1});
  CHECK(inserted) << "Vulkan residency provider id collision";
  return id;
}

void ZVulkanResidencyManager::unregisterResourceProvider(ResourceProviderId id)
{
  if (id == 0) {
    return;
  }
  std::scoped_lock lock(m_mutex);
  auto it = m_resourceProviders.find(id);
  if (it == m_resourceProviders.end()) {
    return;
  }
  CHECK(it->second.refCount > 0u) << "Vulkan residency provider refcount underflow";
  it->second.refCount--;
  if (it->second.refCount == 0u) {
    m_resourceProviders.erase(it);
  }
}

std::vector<ZVulkanResidencyManager::ResourceReport> ZVulkanResidencyManager::memoryReports() const
{
  std::vector<ResourceProvider> providers;
  std::vector<ResourceReport> reports;
  {
    std::scoped_lock lock(m_mutex);
    reports.push_back(managedTextureReportLocked(ResourceClass::DenseImageTexture));
    reports.push_back(managedTextureReportLocked(ResourceClass::DenseVolumeTexture));
    reports.push_back(managedTextureReportLocked(ResourceClass::PagedImageCacheR8));
    providers.reserve(m_resourceProviders.size());
    for (const auto& [_, record] : m_resourceProviders) {
      providers.push_back(record.provider);
    }
  }

  for (const auto& provider : providers) {
    if (provider.report) {
      reports.push_back(provider.report());
    }
  }
  return reports;
}

std::string ZVulkanResidencyManager::describeMemoryByClass() const
{
  struct Aggregate
  {
    uint32_t residentObjects = 0;
    uint32_t pinnedObjects = 0;
    uint64_t residentBytes = 0;
  };

  std::map<ResourceClass, Aggregate> aggregates;
  for (const ResourceReport& report : memoryReports()) {
    auto& aggregate = aggregates[report.resourceClass];
    aggregate.residentObjects += report.residentObjects;
    aggregate.pinnedObjects += report.pinnedObjects;
    aggregate.residentBytes += report.residentBytes;
  }

  std::string out;
  bool first = true;
  for (const auto& [resourceClass, aggregate] : aggregates) {
    if (!first) {
      out += "; ";
    }
    first = false;
    out += fmt::format("{}: {} objects / {} pinned / {:.2f} MiB",
                       resourceClassName(resourceClass),
                       aggregate.residentObjects,
                       aggregate.pinnedObjects,
                       static_cast<double>(aggregate.residentBytes) / (1024.0 * 1024.0));
  }
  return out.empty() ? std::string("<empty>") : out;
}

std::string ZVulkanResidencyManager::describeMemoryByLabel() const
{
  auto reports = memoryReports();
  std::sort(reports.begin(), reports.end(), [](const ResourceReport& a, const ResourceReport& b) {
    if (a.residentBytes != b.residentBytes) {
      return a.residentBytes > b.residentBytes;
    }
    if (a.resourceClass != b.resourceClass) {
      return static_cast<uint8_t>(a.resourceClass) < static_cast<uint8_t>(b.resourceClass);
    }
    return a.label < b.label;
  });

  std::string out;
  bool first = true;
  for (const ResourceReport& report : reports) {
    if (report.residentBytes == 0u && report.residentObjects == 0u && report.pinnedObjects == 0u) {
      continue;
    }
    if (!first) {
      out += "; ";
    }
    first = false;
    out += fmt::format("{}({}): {} objects / {} pinned / {:.2f} MiB",
                       report.label.empty() ? std::string(resourceClassName(report.resourceClass)) : report.label,
                       resourceClassName(report.resourceClass),
                       report.residentObjects,
                       report.pinnedObjects,
                       static_cast<double>(report.residentBytes) / (1024.0 * 1024.0));
  }
  return out.empty() ? std::string("<empty>") : out;
}

uint64_t ZVulkanResidencyManager::effectiveBrokerBudgetBytes() const
{
  std::scoped_lock lock(m_mutex);
  return effectiveResidencyBudgetBytesLocked();
}

bool ZVulkanResidencyManager::strictBudgetActive() const
{
  return strictResidencyBudgetActive();
}

ZVulkanResidencyManager::ReclaimStats ZVulkanResidencyManager::reclaimManagedTexturesLocked(ResourceClass resourceClass,
                                                                                            uint64_t targetBytes,
                                                                                            std::string_view reason)
{
  CHECK(isManagedTextureResourceClass(resourceClass))
    << "reclaimManagedTexturesLocked called for non-texture resource class: " << resourceClassName(resourceClass);

  ReclaimStats stats{};
  while (targetBytes == 0u || stats.bytesReleased < targetBytes) {
    ManagedTexture* best = nullptr;
    uint64_t bestTick = std::numeric_limits<uint64_t>::max();
    for (auto& [_, rec] : m_textures) {
      if (!rec.texture || !rec.texture->resident()) {
        continue;
      }
      if (rec.pinCount > 0u) {
        continue;
      }
      if (textureProtectedLocked(rec)) {
        continue;
      }
      if (resourceClassForTextureKind(rec.key.kind) != resourceClass) {
        continue;
      }
      const uint64_t tick = rec.lastUsedTick;
      if (tick != 0 && tick < bestTick) {
        bestTick = tick;
        best = &rec;
      }
    }
    if (best == nullptr) {
      break;
    }

    const uint64_t bytes = best->texture->allocationSizeBytes();
    evictLocked(*best);
    stats.resourcesReleased++;
    stats.bytesReleased += bytes;
  }

  if (stats.resourcesReleased > 0u) {
    VLOG(1) << fmt::format("VK managed texture pressure reclaim: class={} reason='{}' resources={} bytes={}B",
                           resourceClassName(resourceClass),
                           reason.empty() ? std::string("<unspecified>") : std::string(reason),
                           stats.resourcesReleased,
                           stats.bytesReleased);
  }
  return stats;
}

ZVulkanResidencyManager::ResourceReport
ZVulkanResidencyManager::managedTextureReportLocked(ResourceClass resourceClass) const
{
  CHECK(isManagedTextureResourceClass(resourceClass))
    << "managedTextureReportLocked called for non-texture resource class: " << resourceClassName(resourceClass);

  ResourceReport report{};
  report.resourceClass = resourceClass;
  report.label = std::string(resourceClassName(resourceClass));
  for (const auto& [_, rec] : m_textures) {
    if (resourceClassForTextureKind(rec.key.kind) != resourceClass) {
      continue;
    }
    if (!rec.texture || !rec.texture->resident()) {
      continue;
    }
    report.residentObjects++;
    if (rec.pinCount > 0u) {
      report.pinnedObjects++;
    }
    report.residentBytes += rec.texture->allocationSizeBytes();
  }
  return report;
}

void ZVulkanResidencyManager::releaseOwner(const void* owner)
{
  if (owner == nullptr) {
    return;
  }
  std::scoped_lock lock(m_mutex);

  for (auto it = m_textures.begin(); it != m_textures.end();) {
    if (it->first.owner != owner) {
      ++it;
      continue;
    }
    CHECK(it->second.pinCount == 0u) << "Attempted to release managed textures while pinned (in-flight use); "
                                        "callers must flushForTeardown()/waitForAllInFlight before releaseOwner";
    if (it->second.texture) {
      CHECK(!textureProtectedLocked(it->second)) << "Attempted to release managed texture while protected";
      m_reverse.erase(it->second.texture.get());
    }
    it = m_textures.erase(it);
  }
  m_imageOwnerCallbacksRegistered.erase(owner);
}

void ZVulkanResidencyManager::registerImageOwnerDestructionCallbackLocked(Z3DImg& owner)
{
  const void* ownerKey = &owner;
  if (!m_imageOwnerCallbacksRegistered.insert(ownerKey).second) {
    return;
  }

  std::weak_ptr<std::atomic_bool> alive = m_aliveFlag;
  ZVulkanDevice* device = &m_device;
  owner.addDestructionCallback([alive, device, ownerKey]() {
    auto alivePtr = alive.lock();
    if (!alivePtr || !alivePtr->load(std::memory_order_relaxed)) {
      return;
    }
    CHECK(device != nullptr) << "Vulkan residency destruction callback missing device";
    device->residencyManager().releaseOwner(ownerKey);
  });
}

uint64_t ZVulkanResidencyManager::evictLeastRecentlyUsedLocked(const TextureKey& excludeKey)
{
  ManagedTexture* best = nullptr;
  uint64_t bestTick = std::numeric_limits<uint64_t>::max();

  for (auto& [key, rec] : m_textures) {
    if (key == excludeKey) {
      continue;
    }
    if (!rec.texture || !rec.texture->resident()) {
      continue;
    }
    if (rec.pinCount > 0u) {
      continue;
    }
    if (textureProtectedLocked(rec)) {
      continue;
    }
    const uint64_t tick = rec.lastUsedTick;
    if (tick != 0 && tick < bestTick) {
      bestTick = tick;
      best = &rec;
    }
  }

  if (!best) {
    return 0u;
  }
  const uint64_t releasedBytes = best->texture->allocationSizeBytes();
  evictLocked(*best);
  return releasedBytes;
}

void ZVulkanResidencyManager::evictForBudgetLocked(const TextureKey& excludeKey,
                                                   ResourceClass requestClass,
                                                   uint64_t bytesNeeded,
                                                   std::string_view reason)
{
  const AllocationPressure pressure = allocationPressureForLocked(bytesNeeded);
  if (!pressure.needsReclaim()) {
    return;
  }

  ReclaimStats stats{};
  while (stats.bytesReleased < pressure.reclaimBytes) {
    const uint64_t releasedBytes = evictLeastRecentlyUsedLocked(excludeKey);
    if (releasedBytes == 0u) {
      break;
    }
    stats.resourcesReleased++;
    stats.bytesReleased += releasedBytes;
  }

  if (strictResidencyBudgetActive()) {
    const AllocationPressure retryPressure = allocationPressureForLocked(bytesNeeded);
    if (retryPressure.needsReclaim()) {
      throw ZException(fmt::format(
        "VK residency budget exceeded before managed texture allocation: class={} requested={}B usage={}B budget={}B unreclaimed={}B reason='{}'",
        resourceClassName(requestClass),
        bytesNeeded,
        retryPressure.usageBytes,
        retryPressure.budgetBytes,
        retryPressure.reclaimBytes,
        reason.empty() ? "<unspecified>" : std::string(reason)));
    }
  }
}

void ZVulkanResidencyManager::enforceBudgetAfterManagedAllocationLocked(ResourceClass requestClass,
                                                                        uint64_t allocationBytes,
                                                                        std::string_view reason) const
{
  if (!strictResidencyBudgetActive()) {
    return;
  }

  const uint64_t budgetBytes = effectiveResidencyBudgetBytesLocked();
  const auto budget = m_device.deviceLocalBudget();
  const uint64_t usageBytes = managedResidentBytesLocked();
  if (budgetBytes == 0u && budget.budgetBytes == 0u && usageBytes == 0u) {
    return;
  }
  if (usageBytes <= budgetBytes) {
    return;
  }

  throw ZException(fmt::format(
    "VK residency budget exceeded after managed texture allocation: class={} requested={}B usage={}B budget={}B unreclaimed={}B reason='{}'",
    resourceClassName(requestClass),
    allocationBytes,
    usageBytes,
    budgetBytes,
    usageBytes - budgetBytes,
    reason.empty() ? "<unspecified>" : std::string(reason)));
}

void ZVulkanResidencyManager::evictLocked(ManagedTexture& victim)
{
  CHECK(victim.texture != nullptr);
  CHECK(victim.texture->resident());
  CHECK(victim.pinCount == 0u);
  CHECK(!textureProtectedLocked(victim)) << "Attempted to evict a protected managed texture";
  DeviceAllocationRecoveryScope allocationRecoveryScope(m_device);

  if (isHostBackedDenseTextureKind(victim.key.kind)) {
    VLOG(1) << fmt::format("VK evict dense image: kind={} owner={} idx={} bytes={} tick={}",
                           textureKindName(victim.key.kind),
                           victim.key.owner,
                           victim.key.index,
                           victim.texture->allocationSizeBytes(),
                           victim.lastUsedTick);
    victim.texture->releaseDeviceResources();
    return;
  }

  CHECK(victim.key.kind == TextureKind::PagedImageCacheR8)
    << "Unknown managed Vulkan texture kind during eviction: " << textureKindName(victim.key.kind);

  // Preserve contents in host memory when needed.
  const bool needsHostBackup = victim.dirty;
  const bool canSkipBackup = victim.knownZero && !victim.dirty;

  if (canSkipBackup) {
    // Known-zero contents: no host backup required.
    victim.hostData.clear();
    victim.hostValid = false;
    victim.dirty = false;
    victim.knownZero = true;
  } else if (!needsHostBackup && victim.hostValid && !victim.hostData.empty()) {
    // Host backup is already valid; keep it.
    victim.dirty = false;
    victim.knownZero = false;
  } else {
    const uint64_t bytes = pagedImageCacheByteSizeFor(*victim.texture);
    CHECK(bytes > 0) << "Invalid cache byte size for eviction";
    CHECK(bytes <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) << "Cache too large for host backup";
    victim.hostData.resize(static_cast<size_t>(bytes));
    VLOG(1) << fmt::format("VK evict paged imageCache: owner={} idx={} bytes={} dirty={} hostValid={} tick={}",
                           victim.key.owner,
                           victim.key.index,
                           bytes,
                           victim.dirty ? 1 : 0,
                           victim.hostValid ? 1 : 0,
                           victim.lastUsedTick);
    victim.texture->downloadData(victim.hostData.data(), victim.hostData.size());
    victim.hostValid = true;
    victim.dirty = false;
    victim.knownZero = false;
  }

  victim.texture->releaseDeviceResources();
}

bool ZVulkanResidencyManager::textureProtectedLocked(const ManagedTexture& record) const
{
  return record.texture != nullptr && m_protectedTextures.find(record.texture.get()) != m_protectedTextures.end();
}

void ZVulkanResidencyManager::releaseTextureProtections(std::span<ZVulkanTexture* const> textures)
{
  if (textures.empty()) {
    return;
  }

  std::scoped_lock lock(m_mutex);
  for (auto* texture : textures) {
    if (texture == nullptr) {
      continue;
    }
    auto it = m_protectedTextures.find(texture);
    CHECK(it != m_protectedTextures.end()) << "Managed texture protection release underflow";
    CHECK_GT(it->second, 0u) << "Managed texture protection count underflow";
    it->second--;
    if (it->second == 0u) {
      m_protectedTextures.erase(it);
    }
  }
}

void ZVulkanResidencyManager::ensurePagedImageCacheResidentLocked(const TextureKey& key,
                                                                  ManagedTexture& record,
                                                                  glm::uvec3 cacheSize)
{
  DeviceAllocationRecoveryScope allocationRecoveryScope(m_device);
  CHECK(record.key == key);
  CHECK(cacheSize.x > 0u && cacheSize.y > 0u && cacheSize.z > 0u) << "Invalid Vulkan imageCacheSize";

  // Best-effort byte estimate for budget pre-eviction. Actual VkImage allocations
  // may be larger; OOM-driven eviction retries remain authoritative.
  const uint64_t neededBytes = static_cast<uint64_t>(cacheSize.x) * cacheSize.y * std::max<uint32_t>(1u, cacheSize.z);

  record.lastUsedTick = m_usageTick++;

  if (!record.texture) {
    // Create the cache texture under memory pressure; evict and retry on failure.
    evictForBudgetLocked(key, ResourceClass::PagedImageCacheR8, neededBytes, "paged_image_cache_create");

    uint32_t attempt = 0;
    while (true) {
      try {
        record.texture = createPagedImageCacheTexture(m_device, cacheSize);
        CHECK(record.texture) << "Failed to create Vulkan paged image cache texture";
        enforceBudgetAfterManagedAllocationLocked(ResourceClass::PagedImageCacheR8,
                                                  neededBytes,
                                                  "paged_image_cache_create_postallocate");
        m_reverse.emplace(record.texture.get(), key);

        // Initialize contents deterministically (either restore from host or clear).
        if (record.hostValid && !record.hostData.empty()) {
          record.texture->uploadData(record.hostData.data(),
                                     record.hostData.size(),
                                     vk::ImageLayout::eShaderReadOnlyOptimal);
          record.knownZero = false;
        } else {
          clearPagedImageCacheToZero(m_device, *record.texture);
          record.knownZero = true;
        }
        record.dirty = false;
        record.hostValid = record.hostValid && !record.hostData.empty();
        return;
      }
      catch (const std::exception& e) {
        LOG(WARNING) << fmt::format("VK paged imageCache create failed (attempt {}): {}", attempt++, e.what());
        if (record.texture) {
          m_reverse.erase(record.texture.get());
          record.texture.reset();
        }
        if (evictLeastRecentlyUsedLocked(key) == 0u) {
          throw;
        }
      }
    }
  }

  if (record.texture->resident()) {
    return;
  }

  evictForBudgetLocked(key, ResourceClass::PagedImageCacheR8, neededBytes, "paged_image_cache_recreate");

  uint32_t attempt = 0;
  while (true) {
    try {
      record.texture->recreateDeviceResources();
      enforceBudgetAfterManagedAllocationLocked(ResourceClass::PagedImageCacheR8,
                                                neededBytes,
                                                "paged_image_cache_recreate_postallocate");
      if (record.hostValid && !record.hostData.empty()) {
        record.texture->uploadData(record.hostData.data(),
                                   record.hostData.size(),
                                   vk::ImageLayout::eShaderReadOnlyOptimal);
        record.knownZero = false;
      } else {
        clearPagedImageCacheToZero(m_device, *record.texture);
        record.knownZero = true;
      }
      record.dirty = false;
      return;
    }
    catch (const std::exception& e) {
      LOG(WARNING) << fmt::format("VK paged imageCache recreate failed (attempt {}): {}", attempt++, e.what());
      if (record.texture && record.texture->resident()) {
        record.texture->releaseDeviceResources();
      }
      if (evictLeastRecentlyUsedLocked(key) == 0u) {
        throw;
      }
    }
  }
}

void ZVulkanResidencyManager::ensureDenseImageResidentLocked(const TextureKey& key,
                                                             ManagedTexture& record,
                                                             std::shared_ptr<const ZImg> image,
                                                             uint64_t generation)
{
  DeviceAllocationRecoveryScope allocationRecoveryScope(m_device);
  CHECK(record.key == key);
  CHECK(isHostBackedDenseTextureKind(key.kind)) << "ensureDenseImageResidentLocked called for wrong texture kind";
  CHECK(image != nullptr) << "Dense Vulkan image source image missing";
  CHECK(image->info().bytesPerVoxel == 1u) << "Dense Vulkan image source must be R8";
  CHECK(image->numChannels() == 1u) << "Dense Vulkan image source must be a single-channel ZImg";

  const glm::uvec3 size{static_cast<uint32_t>(image->width()),
                        static_cast<uint32_t>(image->height()),
                        key.kind == TextureKind::DenseImage2DR8 ? 1u : static_cast<uint32_t>(image->depth())};
  CHECK(size.x > 0u && size.y > 0u && size.z > 0u) << "Dense Vulkan image source has empty extent";
  CHECK(key.kind != TextureKind::DenseImage2DR8 || image->depth() == 1)
    << "Dense Vulkan 2D image source must have depth=1";
  const uint64_t neededBytes = denseR8ImageByteSizeFor(size);
  const size_t byteSize = image->byteNumber();
  CHECK(static_cast<uint64_t>(byteSize) == neededBytes)
    << "Dense Vulkan image byte count does not match R8 extent: bytes=" << byteSize << " expected=" << neededBytes;
  const uint8_t* data = image->channelData<uint8_t>(0);
  CHECK(data != nullptr) << "Dense Vulkan image source data missing";

  const bool sizeChanged = record.logicalSize != size;
  if (sizeChanged) {
    CHECK(record.pinCount == 0u) << "Attempted to resize managed dense image while pinned (in-flight use)";
    if (record.texture) {
      m_reverse.erase(record.texture.get());
      record.texture.reset();
    }
    record.logicalSize = size;
    record.sourceGeneration = 0;
  }

  record.sourceImage = std::move(image);
  record.lastUsedTick = m_usageTick++;

  auto uploadSource = [&]() {
    CHECK(record.texture != nullptr);
    CHECK(record.texture->resident()) << "Dense Vulkan image upload requires resident texture";
    record.texture->uploadData(data, byteSize, vk::ImageLayout::eShaderReadOnlyOptimal);
    record.sourceGeneration = generation;
    record.dirty = false;
    record.knownZero = false;
    record.hostValid = true;
  };

  const ResourceClass resourceClass = resourceClassForTextureKind(key.kind);
  const char* kindLabel = textureKindName(key.kind);

  if (!record.texture) {
    evictForBudgetLocked(key, resourceClass, neededBytes, "dense_image_create");

    uint32_t attempt = 0;
    while (true) {
      try {
        record.texture = key.kind == TextureKind::DenseImage2DR8 ? createDenseImage2DTexture(m_device, size)
                                                                 : createDenseVolumeTexture(m_device, size);
        CHECK(record.texture != nullptr) << "Failed to create Vulkan dense image texture";
        enforceBudgetAfterManagedAllocationLocked(resourceClass, neededBytes, "dense_image_create_postallocate");
        m_reverse.emplace(record.texture.get(), key);
        uploadSource();
        return;
      }
      catch (const std::exception& e) {
        LOG(WARNING) << fmt::format("VK dense image create failed (kind={} owner={} idx={} attempt {}): {}",
                                    kindLabel,
                                    key.owner,
                                    key.index,
                                    attempt++,
                                    e.what());
        if (record.texture) {
          m_reverse.erase(record.texture.get());
          record.texture.reset();
        }
        if (evictLeastRecentlyUsedLocked(key) == 0u) {
          throw;
        }
      }
    }
  }

  if (!record.texture->resident()) {
    evictForBudgetLocked(key, resourceClass, neededBytes, "dense_image_recreate");

    uint32_t attempt = 0;
    while (true) {
      try {
        record.texture->recreateDeviceResources();
        enforceBudgetAfterManagedAllocationLocked(resourceClass, neededBytes, "dense_image_recreate_postallocate");
        uploadSource();
        return;
      }
      catch (const std::exception& e) {
        LOG(WARNING) << fmt::format("VK dense image recreate failed (kind={} owner={} idx={} attempt {}): {}",
                                    kindLabel,
                                    key.owner,
                                    key.index,
                                    attempt++,
                                    e.what());
        if (record.texture && record.texture->resident()) {
          record.texture->releaseDeviceResources();
        }
        if (evictLeastRecentlyUsedLocked(key) == 0u) {
          throw;
        }
      }
    }
  }

  if (record.sourceGeneration != generation) {
    CHECK(record.pinCount == 0u) << "Attempted to reupload managed dense image while pinned (in-flight use)";
    uploadSource();
  }
}

} // namespace nim
