#include "zvulkanpagedimageblockuploader.h"

#include "z3drenderervulkanbackend.h"
#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"
#include "zrenderthreadexecutor_tls.h"
#include "zvulkandevice.h"
#include "zvulkanresidencymanager.h"
#include "zvulkantexture.h"

#include <folly/OperationCancelled.h>
#include <folly/MPMCQueue.h>
#include <folly/ScopeGuard.h>
#include <folly/coro/FutureUtil.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>

#include <algorithm>
#include <chrono>
#include <limits>

DECLARE_uint32(atlas_log_folly_global_executor_status_interval_in_seconds);
DECLARE_uint32(atlas_3d_paging_queue_poll_interval_ms);

namespace nim {

namespace {

vk::SamplerCreateInfo makeNearestSampler()
{
  vk::SamplerCreateInfo sampler{};
  sampler.magFilter = vk::Filter::eNearest;
  sampler.minFilter = vk::Filter::eNearest;
  sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
  sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  sampler.unnormalizedCoordinates = false;
  sampler.borderColor = vk::BorderColor::eIntOpaqueBlack;
  sampler.compareEnable = false;
  sampler.anisotropyEnable = false;
  sampler.maxAnisotropy = 1.0f;
  sampler.minLod = 0.0f;
  sampler.maxLod = 0.0f;
  return sampler;
}

std::unique_ptr<ZVulkanTexture> createUint3DTexture(ZVulkanDevice& device, glm::uvec3 size)
{
  if (size.x == 0u || size.y == 0u || size.z == 0u) {
    return nullptr;
  }

  auto info =
    ZVulkanTexture::CreateInfo::make3D(size.x,
                                       size.y,
                                       size.z,
                                       vk::Format::eR32G32B32A32Uint,
                                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                       vk::MemoryPropertyFlagBits::eDeviceLocal,
                                       1u,
                                       false,
                                       vk::ImageLayout::eShaderReadOnlyOptimal);
  info.createDefaultSampler = false;
  info.residencyClassHint = ZVulkanTexture::ResidencyClassHint::PagedImageMetadataTexture;
  info.samplerInfo = makeNearestSampler();
  return device.createTexture(info);
}

} // namespace

ZVulkanPagedImageBlockUploader::ZVulkanPagedImageBlockUploader(ZVulkanDevice& device)
  : m_device(device)
  , m_aliveFlag(std::make_shared<std::atomic_bool>(true))
{
  m_metadataProviderId = m_device.residencyManager().registerResourceProvider(ZVulkanResidencyManager::ResourceProvider{
    .resourceClass = ZVulkanResidencyManager::ResourceClass::PagedImageMetadataTexture,
    .priority = 0,
    .owner = this,
    .label = "paged_image_metadata",
    .collectCandidates =
      [this]() {
        return metadataEvictionCandidates();
      },
    .evictCandidate =
      [this](const ZVulkanResidencyManager::EvictionCandidate& candidate,
             const ZVulkanResidencyManager::ReclaimRequest& request) {
        return evictMetadataCandidate(candidate, request.reason);
      },
    .report =
      [this]() {
        return metadataMemoryReport();
      },
  });
}

ZVulkanPagedImageBlockUploader::~ZVulkanPagedImageBlockUploader()
{
  if (m_metadataProviderId != 0u) {
    m_device.residencyManager().unregisterResourceProvider(m_metadataProviderId);
    m_metadataProviderId = 0u;
  }

  std::vector<const Z3DImg*> owners;
  {
    std::scoped_lock lock(m_mutex);
    owners.reserve(m_resources.size());
    for (const auto& [image, _] : m_resources) {
      owners.push_back(image);
    }

    // Mark callbacks inactive before dropping the uploader so late Z3DImg
    // destruction callbacks do not touch this uploader after it is gone.
    m_aliveFlag->store(false, std::memory_order_relaxed);
    m_resources.clear();
  }

  // The residency manager owns the paged image-cache textures. If we are being
  // destroyed during a Vulkan -> OpenGL backend switch, the corresponding Z3DImg
  // objects may outlive this uploader, so their destruction callbacks can no
  // longer be relied upon to release residency-managed textures.
  //
  // Invariant: backend-switch / teardown paths must have drained Vulkan work
  // first via preBackendSwitch()/flushForTeardown(), so no managed cache owned
  // by these images remains pinned when releaseOwner() runs below.
  for (const Z3DImg* owner : owners) {
    m_device.residencyManager().releaseOwner(owner);
  }
}

void ZVulkanPagedImageBlockUploader::ensureImageResources(Z3DImg& image)
{
  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  resources.channels.resize(image.numChannels());
  for (size_t idx = 0; idx < resources.channels.size(); ++idx) {
    ensureChannelResourcesLocked(image, idx, resources.channels[idx]);
  }
}

size_t ZVulkanPagedImageBlockUploader::readAndUploadImageBlocks(
  Z3DImg& image,
  size_t channel,
  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
  const folly::CancellationToken& cancellationToken,
  ZBenchTimer& timer)
{
  if (pendingTasks.empty()) {
    return 0;
  }

  // Ensure the paging resources (page directory/table) exist for this image.
  // The image-cache residency itself is handled by the device-owned residency manager.
  ensureImageResources(image);

  CHECK(channel <= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) << "Channel index too large";
  const auto cacheSize = image.imageCacheSize();
  ZVulkanTexture* imageCacheTexture =
    m_device.residencyManager().pagedImageCacheTexture(&image, static_cast<uint32_t>(channel), cacheSize);

  CHECK(imageCacheTexture) << "Vulkan image cache texture missing for channel " << channel;

  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto* threadPool = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get());
  CHECK(threadPool != nullptr);

  const glm::uvec3 extent = image.imageBlockExtent();
  const size_t bytesPerBlock = image.imageBlockByteSize();
  const ZImgInfo resInfo(extent.x, extent.y, extent.z, 1);

  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<ZImg>, std::optional<std::string>>, true> imgQueue;
  auto readFuture =
    folly::coro::toFuture(folly::coro::co_withCancellation(
                            cancellationToken,
                            image.readImageBlocksToQueueAsync(channel, pendingTasks, resInfo, imgQueue, timer)),
                          cpuExecutor);
  // The reader coroutine captures references to imgQueue/pendingTasks/timer. If we return early (e.g. cancellation),
  // we must wait for it to finish so background tasks don't enqueue into a destroyed queue, corrupting memory.
  auto readFutureGuard = folly::makeGuard([&]() {
    if (!readFuture.valid()) {
      return;
    }
    readFuture.wait();
    if (!readFuture.hasException()) {
      return;
    }
    const auto& result = readFuture.result();
    if (result.hasException<ZCancellationException>() || result.hasException<folly::OperationCancelled>()) {
      // Cancellation is expected during progressive rendering. Avoid noisy logs.
      VLOG(2) << "Vulkan paged-image block reader cancelled.";
      return;
    }
    LOG(ERROR) << "Vulkan paged-image block reader failed: " << result.exception().what();
  });

  size_t emptyBlockCount = 0;
  int remainingBlocks = static_cast<int>(pendingTasks.size());
  std::tuple<size_t, std::shared_ptr<ZImg>, std::optional<std::string>> elem;
  auto lastLog = std::chrono::steady_clock::now();
  bool markedDirty = false;

  while (remainingBlocks > 0) {
    maybeCancel(cancellationToken);

    if (imgQueue.try_dequeue_until(
          elem,
          std::chrono::steady_clock::now() + std::chrono::milliseconds(FLAGS_atlas_3d_paging_queue_poll_interval_ms))) {
      const auto taskIndex = std::get<0>(elem);
      const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[taskIndex];

      if (std::get<2>(elem).has_value()) {
        image.recordPagingFailure(pageTableEntryKey, *std::get<2>(elem));
      }

      if (!std::get<1>(elem)) {
        ++emptyBlockCount;
        *pageTableEntryPtr = image.emptyPageTableEntry();
      } else {
        image.mapImageBlockToCache(channel, pageTableEntryKey, *pageTableEntryPtr);

        const glm::uvec3 cacheOffset(pageTableEntryPtr->x, pageTableEntryPtr->y, pageTableEntryPtr->z);
        ZVulkanTexture::UploadRegion uploadRegion{};
        uploadRegion.offset = vk::Offset3D{static_cast<int32_t>(cacheOffset.x),
                                           static_cast<int32_t>(cacheOffset.y),
                                           static_cast<int32_t>(cacheOffset.z)};
        uploadRegion.extent = vk::Extent3D{extent.x, extent.y, extent.z};
        uploadRegion.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

        if (!markedDirty) {
          m_device.residencyManager().notifyPagedImageCacheWritten(&image, static_cast<uint32_t>(channel));
          markedDirty = true;
        }
        imageCacheTexture->uploadSubImage(std::get<1>(elem)->channelData(0), bytesPerBlock, uploadRegion);
      }

      --remainingBlocks;
    } else if (readFuture.isReady() && imgQueue.size() == 0) {
      // Reader finished but no more blocks will arrive. Break to surface the underlying error
      // (or crash if it finished "successfully" but failed to enqueue all blocks).
      break;
    }

    if (std::chrono::steady_clock::now() - lastLog >=
        std::chrono::seconds(FLAGS_atlas_log_folly_global_executor_status_interval_in_seconds)) {
      auto stats = threadPool->getPoolStats();
      LOG(INFO) << fmt::format(
        "pending/total task count: {}/{}, active/idle thread count: {}/{}, ready/remaining blocks: {}/{}",
        stats.pendingTaskCount,
        stats.totalTaskCount,
        stats.activeThreadCount,
        stats.idleThreadCount,
        imgQueue.size(),
        remainingBlocks);
      lastLog = std::chrono::steady_clock::now();
    }
  }

  // Ensure the reader finished and propagate any unexpected failures.
  std::move(readFuture).get();
  CHECK(remainingBlocks == 0) << "Vulkan block reader ended before delivering all blocks";

  timer.recordEvent("image blocks uploading");
  VLOG(2) << "Uploaded " << (pendingTasks.size() - emptyBlockCount) << " image blocks to Vulkan image cache.";
  return emptyBlockCount;
}

void ZVulkanPagedImageBlockUploader::uploadPageCaches(Z3DImg& image, size_t channel, ZBenchTimer& timer)
{
  VLOG(2) << "Uploading page caches for channel " << channel;
  ZVulkanTexture* pageDirectory = nullptr;
  ZVulkanTexture* pageTableCache = nullptr;

  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    resources.channels.resize(image.numChannels());
    CHECK_LT(channel, resources.channels.size());
    ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
    pageDirectory = resources.channels[channel].pageDirectory.get();
    pageTableCache = resources.channels[channel].pageTableCache.get();
  }

  VLOG(2) << "Uploading page directory and page table cache textures";

  auto pageDirectorySpan = image.pageDirectoryView(channel);
  if (pageDirectory && !pageDirectorySpan.empty()) {
    {
      std::scoped_lock lock(m_mutex);
      auto& resources = ensureImageResourcesLocked(image);
      CHECK_LT(channel, resources.channels.size());
      uint32_t& pinCount = resources.channels[channel].pageDirectoryPinCount;
      CHECK_LT(pinCount, std::numeric_limits<uint32_t>::max()) << "Paged-image directory upload pin count overflow";
      ++pinCount;
    }
    auto uploadPinGuard = folly::makeGuard([this, &image, channel]() {
      unpinMetadataTexture(&image, channel, false);
    });

    if (!pageDirectory->resident()) {
      pageDirectory->recreateDeviceResources();
    }
    pageDirectory->uploadData(pageDirectorySpan.data(),
                              pageDirectorySpan.size_bytes(),
                              vk::ImageLayout::eShaderReadOnlyOptimal);
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    resources.channels[channel].pageDirectoryUploaded = true;
    resources.channels[channel].pageDirectoryLastUsedTick = m_usageTick++;
  }

  auto pageTableSpan = image.pageTableCacheView(channel);
  if (pageTableCache && !pageTableSpan.empty()) {
    {
      std::scoped_lock lock(m_mutex);
      auto& resources = ensureImageResourcesLocked(image);
      CHECK_LT(channel, resources.channels.size());
      uint32_t& pinCount = resources.channels[channel].pageTableCachePinCount;
      CHECK_LT(pinCount, std::numeric_limits<uint32_t>::max()) << "Paged-image page-table upload pin count overflow";
      ++pinCount;
    }
    auto uploadPinGuard = folly::makeGuard([this, &image, channel]() {
      unpinMetadataTexture(&image, channel, true);
    });

    if (!pageTableCache->resident()) {
      pageTableCache->recreateDeviceResources();
    }
    pageTableCache->uploadData(pageTableSpan.data(),
                               pageTableSpan.size_bytes(),
                               vk::ImageLayout::eShaderReadOnlyOptimal);
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    resources.channels[channel].pageTableCacheUploaded = true;
    resources.channels[channel].pageTableCacheLastUsedTick = m_usageTick++;
  }

  timer.recordEvent("upload page table");
}

ZVulkanTexture* ZVulkanPagedImageBlockUploader::pageDirectoryTexture(Z3DImg& image, size_t channel)
{
  ZVulkanTexture* texture = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
    resources.channels[channel].pageDirectoryLastUsedTick = m_usageTick++;
    texture = resources.channels[channel].pageDirectory.get();
  }
  if (texture != nullptr) {
    ensurePageDirectoryResident(image, channel, *texture);
    pinMetadataTextureForActiveSubmission(image, channel, false, *texture);
  }
  return texture;
}

ZVulkanTexture* ZVulkanPagedImageBlockUploader::pageTableTexture(Z3DImg& image, size_t channel)
{
  ZVulkanTexture* texture = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
    resources.channels[channel].pageTableCacheLastUsedTick = m_usageTick++;
    texture = resources.channels[channel].pageTableCache.get();
  }
  if (texture != nullptr) {
    ensurePageTableResident(image, channel, *texture);
    pinMetadataTextureForActiveSubmission(image, channel, true, *texture);
  }
  return texture;
}

ZVulkanTexture* ZVulkanPagedImageBlockUploader::imageCacheTexture(Z3DImg& image, size_t channel)
{
  // Ensure per-image resources exist (page directory/table) and register owner cleanup.
  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
  }

  CHECK(channel <= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) << "Channel index too large";
  const glm::uvec3 cacheSize = image.imageCacheSize();
  ZVulkanTexture* tex =
    m_device.residencyManager().pagedImageCacheTexture(&image, static_cast<uint32_t>(channel), cacheSize);
  if (auto* backend = Z3DRendererVulkanBackend::current()) {
    backend->pinTextureForActiveSubmission(tex);
  }
  return tex;
}

void ZVulkanPagedImageBlockUploader::ensurePageDirectoryResident(Z3DImg& image, size_t channel, ZVulkanTexture& texture)
{
  bool needsUpload = !texture.resident();
  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    needsUpload = needsUpload || !resources.channels[channel].pageDirectoryUploaded;
  }
  if (!needsUpload) {
    return;
  }

  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    uint32_t& pinCount = resources.channels[channel].pageDirectoryPinCount;
    CHECK_LT(pinCount, std::numeric_limits<uint32_t>::max()) << "Paged-image directory restore pin count overflow";
    ++pinCount;
  }
  auto restorePinGuard = folly::makeGuard([this, &image, channel]() {
    unpinMetadataTexture(&image, channel, false);
  });

  if (!texture.resident()) {
    texture.recreateDeviceResources();
  }
  auto pageDirectorySpan = image.pageDirectoryView(channel);
  CHECK(!pageDirectorySpan.empty()) << "Paged-image directory restore source is empty";
  texture.uploadData(pageDirectorySpan.data(), pageDirectorySpan.size_bytes(), vk::ImageLayout::eShaderReadOnlyOptimal);

  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  CHECK_LT(channel, resources.channels.size());
  resources.channels[channel].pageDirectoryUploaded = true;
  resources.channels[channel].pageDirectoryLastUsedTick = m_usageTick++;
}

void ZVulkanPagedImageBlockUploader::ensurePageTableResident(Z3DImg& image, size_t channel, ZVulkanTexture& texture)
{
  bool needsUpload = !texture.resident();
  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    needsUpload = needsUpload || !resources.channels[channel].pageTableCacheUploaded;
  }
  if (!needsUpload) {
    return;
  }

  {
    std::scoped_lock lock(m_mutex);
    auto& resources = ensureImageResourcesLocked(image);
    CHECK_LT(channel, resources.channels.size());
    uint32_t& pinCount = resources.channels[channel].pageTableCachePinCount;
    CHECK_LT(pinCount, std::numeric_limits<uint32_t>::max()) << "Paged-image page-table restore pin count overflow";
    ++pinCount;
  }
  auto restorePinGuard = folly::makeGuard([this, &image, channel]() {
    unpinMetadataTexture(&image, channel, true);
  });

  if (!texture.resident()) {
    texture.recreateDeviceResources();
  }
  auto pageTableSpan = image.pageTableCacheView(channel);
  CHECK(!pageTableSpan.empty()) << "Paged-image page-table restore source is empty";
  texture.uploadData(pageTableSpan.data(), pageTableSpan.size_bytes(), vk::ImageLayout::eShaderReadOnlyOptimal);

  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  CHECK_LT(channel, resources.channels.size());
  resources.channels[channel].pageTableCacheUploaded = true;
  resources.channels[channel].pageTableCacheLastUsedTick = m_usageTick++;
}

void ZVulkanPagedImageBlockUploader::pinMetadataTextureForActiveSubmission(Z3DImg& image,
                                                                           size_t channel,
                                                                           bool pageTable,
                                                                           ZVulkanTexture& texture)
{
  auto* backend = Z3DRendererVulkanBackend::current();
  if (backend == nullptr) {
    return;
  }

  const bool inserted =
    backend->pinExternalResidencyResourceForActiveSubmission(&texture, [this, image = &image, channel, pageTable]() {
      unpinMetadataTexture(image, channel, pageTable);
    });
  if (!inserted) {
    return;
  }

  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  CHECK_LT(channel, resources.channels.size());
  uint32_t& pinCount =
    pageTable ? resources.channels[channel].pageTableCachePinCount : resources.channels[channel].pageDirectoryPinCount;
  CHECK_LT(pinCount, std::numeric_limits<uint32_t>::max()) << "Paged-image metadata pin count overflow";
  ++pinCount;
}

void ZVulkanPagedImageBlockUploader::unpinMetadataTexture(const Z3DImg* image, size_t channel, bool pageTable)
{
  CHECK(image != nullptr) << "Paged-image metadata unpin requires an image owner";

  std::scoped_lock lock(m_mutex);
  auto imageIt = m_resources.find(image);
  CHECK(imageIt != m_resources.end()) << "Paged-image metadata unpin lost image resources";
  CHECK_LT(channel, imageIt->second.channels.size()) << "Paged-image metadata unpin channel out of range";
  uint32_t& pinCount = pageTable ? imageIt->second.channels[channel].pageTableCachePinCount
                                 : imageIt->second.channels[channel].pageDirectoryPinCount;
  CHECK_GT(pinCount, 0u) << "Paged-image metadata pin count underflow";
  --pinCount;
}

void ZVulkanPagedImageBlockUploader::ensureChannelResourcesLocked(Z3DImg& image,
                                                                  size_t channel,
                                                                  ChannelResources& resources)
{
  (void)channel;
  const glm::uvec3 pdSize = image.pageDirectorySize();
  if (resources.pageDirectorySize != pdSize) {
    resources.pageDirectory = createUint3DTexture(m_device, pdSize);
    resources.pageDirectorySize = pdSize;
    resources.pageDirectoryUploaded = false;
    resources.pageDirectoryLastUsedTick = m_usageTick++;
  }

  const glm::uvec3 ptSize = image.pageTableCacheSize();
  if (resources.pageTableCacheSize != ptSize) {
    resources.pageTableCache = createUint3DTexture(m_device, ptSize);
    resources.pageTableCacheSize = ptSize;
    resources.pageTableCacheUploaded = false;
    resources.pageTableCacheLastUsedTick = m_usageTick++;
  }
}

std::vector<ZVulkanResidencyManager::EvictionCandidate>
ZVulkanPagedImageBlockUploader::metadataEvictionCandidates() const
{
  std::vector<ZVulkanResidencyManager::EvictionCandidate> candidates;
  if (currentRenderThreadExecutorOrNull() == nullptr || m_device.frameExecutor().inFlightCount() != 0u) {
    return candidates;
  }

  std::unique_lock lock(m_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return candidates;
  }
  for (const auto& resourceEntry : m_resources) {
    const Z3DImg* image = resourceEntry.first;
    const ImageResources& resources = resourceEntry.second;
    for (size_t channel = 0; channel < resources.channels.size(); ++channel) {
      const auto& channelResources = resources.channels[channel];
      auto addCandidate = [&](const std::unique_ptr<ZVulkanTexture>& texture,
                              bool pageTable,
                              uint64_t lastUsedTick,
                              uint32_t pinCount,
                              std::string_view labelKind) {
        if (!texture || !texture->resident()) {
          return;
        }
        const uint64_t residentBytes = texture->allocationSizeBytes();
        if (residentBytes == 0u) {
          return;
        }
        candidates.push_back(ZVulkanResidencyManager::EvictionCandidate{
          .resourceClass = ZVulkanResidencyManager::ResourceClass::PagedImageMetadataTexture,
          .priority = 0,
          .residentBytes = residentBytes,
          .lastUsedEpoch = lastUsedTick,
          .pinCount = pinCount,
          .restoreAvailable = true,
          .userKey0 = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image)),
          .userKey1 = (static_cast<uint64_t>(channel) << 1u) | (pageTable ? 1u : 0u),
          .label = fmt::format("paged_image_{} owner={} channel={}", labelKind, fmt::ptr(image), channel)});
      };
      addCandidate(channelResources.pageDirectory,
                   false,
                   channelResources.pageDirectoryLastUsedTick,
                   channelResources.pageDirectoryPinCount,
                   "page_directory");
      addCandidate(channelResources.pageTableCache,
                   true,
                   channelResources.pageTableCacheLastUsedTick,
                   channelResources.pageTableCachePinCount,
                   "page_table");
    }
  }
  return candidates;
}

ZVulkanResidencyManager::ReclaimStats
ZVulkanPagedImageBlockUploader::evictMetadataCandidate(const ZVulkanResidencyManager::EvictionCandidate& candidate,
                                                       std::string_view reason)
{
  ZVulkanResidencyManager::ReclaimStats stats{};
  if (candidate.resourceClass != ZVulkanResidencyManager::ResourceClass::PagedImageMetadataTexture ||
      candidate.userKey0 == 0u) {
    return stats;
  }
  if (currentRenderThreadExecutorOrNull() == nullptr || m_device.frameExecutor().inFlightCount() != 0u) {
    return stats;
  }

  const auto* image = reinterpret_cast<const Z3DImg*>(static_cast<uintptr_t>(candidate.userKey0));
  const size_t channel = static_cast<size_t>(candidate.userKey1 >> 1u);
  const bool pageTable = (candidate.userKey1 & 1u) != 0u;

  std::scoped_lock lock(m_mutex);
  auto imageIt = m_resources.find(image);
  if (imageIt == m_resources.end() || channel >= imageIt->second.channels.size()) {
    return stats;
  }
  auto& resources = imageIt->second.channels[channel];
  auto& texture = pageTable ? resources.pageTableCache : resources.pageDirectory;
  const uint32_t pinCount = pageTable ? resources.pageTableCachePinCount : resources.pageDirectoryPinCount;
  if (!texture || !texture->resident() || pinCount > 0u) {
    return stats;
  }

  const uint64_t releasedBytes = texture->allocationSizeBytes();
  texture->releaseDeviceResources();
  stats.resourcesReleased = 1u;
  stats.bytesReleased = releasedBytes;
  VLOG(1) << fmt::format("VK paged-image metadata evict: kind={} owner={} channel={} bytes={}B reason='{}'",
                         pageTable ? "page_table" : "page_directory",
                         fmt::ptr(image),
                         channel,
                         releasedBytes,
                         reason.empty() ? "<unspecified>" : std::string(reason));
  return stats;
}

ZVulkanResidencyManager::ResourceReport ZVulkanPagedImageBlockUploader::metadataMemoryReport() const
{
  ZVulkanResidencyManager::ResourceReport report{};
  report.resourceClass = ZVulkanResidencyManager::ResourceClass::PagedImageMetadataTexture;
  report.label = "paged_image_metadata";

  std::unique_lock lock(m_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return report;
  }
  for (const auto& [_, resources] : m_resources) {
    for (const auto& channelResources : resources.channels) {
      for (const auto* texture : {channelResources.pageDirectory.get(), channelResources.pageTableCache.get()}) {
        if (texture == nullptr || !texture->resident()) {
          continue;
        }
        report.residentObjects++;
        if ((texture == channelResources.pageDirectory.get() && channelResources.pageDirectoryPinCount > 0u) ||
            (texture == channelResources.pageTableCache.get() && channelResources.pageTableCachePinCount > 0u)) {
          report.pinnedObjects++;
        }
        report.residentBytes += texture->allocationSizeBytes();
      }
    }
  }
  return report;
}

ZVulkanPagedImageBlockUploader::ImageResources&
ZVulkanPagedImageBlockUploader::ensureImageResourcesLocked(Z3DImg& image)
{
  auto it = m_resources.find(&image);
  if (it == m_resources.end()) {
    ImageResources resources;
    resources.channels.resize(image.numChannels());
    auto [inserted, _] = m_resources.emplace(&image, std::move(resources));
    it = inserted;
  }

  if (!it->second.destructionRegistered) {
    std::weak_ptr<std::atomic_bool> alive = m_aliveFlag;
    ZVulkanPagedImageBlockUploader* self = this;
    ZVulkanDevice* dev = &m_device;
    image.addDestructionCallback([alive, self, dev, key = &image]() {
      auto alivePtr = alive.lock();
      if (!alivePtr || !alivePtr->load(std::memory_order_relaxed)) {
        return;
      }
      if (dev) {
        dev->residencyManager().releaseOwner(key);
      }
      std::scoped_lock guard(self->m_mutex);
      self->m_resources.erase(key);
    });
    it->second.destructionRegistered = true;
  }

  if (it->second.channels.size() != image.numChannels()) {
    it->second.channels.resize(image.numChannels());
  }
  return it->second;
}

const ZVulkanPagedImageBlockUploader::ImageResources*
ZVulkanPagedImageBlockUploader::findImageResourcesLocked(const Z3DImg& image) const
{
  auto it = m_resources.find(&image);
  if (it == m_resources.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace nim
