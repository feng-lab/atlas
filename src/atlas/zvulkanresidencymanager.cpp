#include "zvulkanresidencymanager.h"

#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanframeexecutor.h"
#include "zvulkantexture.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

DEFINE_uint64(atlas_vk_paged_image_cache_budget_bytes,
              0,
              "Upper bound (bytes) for Vulkan paged image-cache residency. 0 uses the device-reported budget.");

DEFINE_double(atlas_vk_paged_image_cache_budget_ratio,
              1.0,
              "Multiplier applied to the device-reported device-local budget for paged image-cache residency. "
              "Use <1.0 to reserve headroom for non-paging allocations.");

DEFINE_uint64(atlas_vk_paged_image_cache_budget_reserve_bytes,
              0,
              "Reserve this many device-local bytes (subtract from budget) for non-paging allocations.");

namespace nim {

namespace {

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
  info.samplerInfo = makeLinearSampler();
  return std::make_unique<ZVulkanTexture>(device, info);
}

uint64_t pagedImageCacheByteSizeFor(const ZVulkanTexture& texture)
{
  const auto extent = texture.extent();
  CHECK(texture.format() == vk::Format::eR8Unorm)
    << "Only R8 image caches are supported for Vulkan paging swap; fmt=" << enumOrUnderlying(texture.format(), 16);
  return static_cast<uint64_t>(extent.width) * extent.height * std::max<uint32_t>(1u, extent.depth);
}

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
{}

ZVulkanResidencyManager::~ZVulkanResidencyManager() = default;

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
  record.dirty = false;
  record.knownZero = true;
  record.hostValid = false;
  record.lastUsedTick = 0;
  record.hostData.clear();
}

uint64_t ZVulkanResidencyManager::effectivePagedImageCacheBudgetBytesLocked() const
{
  const auto dev = m_device.deviceLocalBudget();
  uint64_t budget = dev.budgetBytes;
  if (budget == 0) {
    // Unknown; caller should fall back to OOM-driven eviction retries.
    return 0;
  }

  if (FLAGS_atlas_vk_paged_image_cache_budget_bytes > 0) {
    budget = std::min<uint64_t>(budget, FLAGS_atlas_vk_paged_image_cache_budget_bytes);
  }

  const double ratioRaw = FLAGS_atlas_vk_paged_image_cache_budget_ratio;
  const double ratio = std::isfinite(ratioRaw) ? std::clamp(ratioRaw, 0.0, 1.0) : 1.0;
  budget = static_cast<uint64_t>(static_cast<double>(budget) * ratio);

  const uint64_t reserve = FLAGS_atlas_vk_paged_image_cache_budget_reserve_bytes;
  if (reserve >= budget) {
    return 0;
  }
  budget -= reserve;
  return budget;
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

  std::scoped_lock lock(m_mutex);
  auto& record = ensureRecordLocked(key);
  resetRecordForSizeLocked(record, cacheSize);
  ensurePagedImageCacheResidentLocked(key, record, cacheSize);
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
    CHECK(it->second.pinCount == 0u) << "Attempted to release managed textures while pinned (in-flight use)";
    if (it->second.texture) {
      m_reverse.erase(it->second.texture.get());
    }
    it = m_textures.erase(it);
  }
}

bool ZVulkanResidencyManager::evictLeastRecentlyUsedLocked(const TextureKey& excludeKey)
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
    const uint64_t tick = rec.lastUsedTick;
    if (tick != 0 && tick < bestTick) {
      bestTick = tick;
      best = &rec;
    }
  }

  if (!best) {
    return false;
  }
  evictLocked(*best);
  return true;
}

void ZVulkanResidencyManager::evictLocked(ManagedTexture& victim)
{
  CHECK(victim.texture != nullptr);
  CHECK(victim.texture->resident());
  CHECK(victim.pinCount == 0u);

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

void ZVulkanResidencyManager::ensurePagedImageCacheResidentLocked(const TextureKey& key,
                                                                  ManagedTexture& record,
                                                                  glm::uvec3 cacheSize)
{
  CHECK(record.key == key);
  CHECK(cacheSize.x > 0u && cacheSize.y > 0u && cacheSize.z > 0u) << "Invalid Vulkan imageCacheSize";

  // Best-effort byte estimate for budget pre-eviction. Actual VkImage allocations
  // may be larger; OOM-driven eviction retries remain authoritative.
  const uint64_t neededBytes = static_cast<uint64_t>(cacheSize.x) * cacheSize.y * std::max<uint32_t>(1u, cacheSize.z);

  record.lastUsedTick = m_usageTick++;

  auto preEvictForBudget = [&](uint64_t bytesNeeded) {
    const uint64_t effectiveBudget = effectivePagedImageCacheBudgetBytesLocked();
    if (effectiveBudget == 0) {
      return;
    }
    uint64_t usage = m_device.deviceLocalBudget().usageBytes;
    while (usage + bytesNeeded > effectiveBudget) {
      if (!evictLeastRecentlyUsedLocked(key)) {
        break;
      }
      usage = m_device.deviceLocalBudget().usageBytes;
    }
  };

  if (!record.texture) {
    // Create the cache texture under memory pressure; evict and retry on failure.
    preEvictForBudget(neededBytes);

    for (int attempt = 0; attempt < 64; ++attempt) {
      try {
        record.texture = createPagedImageCacheTexture(m_device, cacheSize);
        CHECK(record.texture) << "Failed to create Vulkan paged image cache texture";
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
        LOG(WARNING) << fmt::format("VK paged imageCache create failed (attempt {}): {}", attempt, e.what());
        if (!evictLeastRecentlyUsedLocked(key)) {
          throw;
        }
      }
    }

    throw ZException("VK failed to create paged image cache after repeated eviction attempts");
  }

  if (record.texture->resident()) {
    return;
  }

  preEvictForBudget(neededBytes);

  for (int attempt = 0; attempt < 64; ++attempt) {
    try {
      record.texture->recreateDeviceResources();
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
      LOG(WARNING) << fmt::format("VK paged imageCache recreate failed (attempt {}): {}", attempt, e.what());
      if (!evictLeastRecentlyUsedLocked(key)) {
        throw;
      }
    }
  }

  throw ZException("VK failed to recreate paged image cache after repeated eviction attempts");
}

} // namespace nim
