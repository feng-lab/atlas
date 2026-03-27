#include "zvulkanpagedimageblockuploader.h"

#include "z3drenderervulkanbackend.h"
#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkanresidencymanager.h"
#include "zvulkantexture.h"

#include <folly/OperationCancelled.h>
#include <folly/MPMCQueue.h>
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
  info.samplerInfo = makeNearestSampler();
  return std::make_unique<ZVulkanTexture>(device, info);
}

} // namespace

ZVulkanPagedImageBlockUploader::ZVulkanPagedImageBlockUploader(ZVulkanDevice& device)
  : m_device(device)
  , m_aliveFlag(std::make_shared<std::atomic_bool>(true))
{}

ZVulkanPagedImageBlockUploader::~ZVulkanPagedImageBlockUploader()
{
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
    processEventsAndMaybeCancel(cancellationToken);

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
    pageDirectory->uploadData(pageDirectorySpan.data(),
                              pageDirectorySpan.size_bytes(),
                              vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  auto pageTableSpan = image.pageTableCacheView(channel);
  if (pageTableCache && !pageTableSpan.empty()) {
    pageTableCache->uploadData(pageTableSpan.data(),
                               pageTableSpan.size_bytes(),
                               vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  timer.recordEvent("upload page table");
}

ZVulkanTexture* ZVulkanPagedImageBlockUploader::pageDirectoryTexture(Z3DImg& image, size_t channel)
{
  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  CHECK_LT(channel, resources.channels.size());
  ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
  return resources.channels[channel].pageDirectory.get();
}

ZVulkanTexture* ZVulkanPagedImageBlockUploader::pageTableTexture(Z3DImg& image, size_t channel)
{
  std::scoped_lock lock(m_mutex);
  auto& resources = ensureImageResourcesLocked(image);
  CHECK_LT(channel, resources.channels.size());
  ensureChannelResourcesLocked(image, channel, resources.channels[channel]);
  return resources.channels[channel].pageTableCache.get();
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

void ZVulkanPagedImageBlockUploader::ensureChannelResourcesLocked(Z3DImg& image,
                                                                  size_t channel,
                                                                  ChannelResources& resources)
{
  (void)channel;
  const glm::uvec3 pdSize = image.pageDirectorySize();
  if (resources.pageDirectorySize != pdSize) {
    resources.pageDirectory = createUint3DTexture(m_device, pdSize);
    resources.pageDirectorySize = pdSize;
  }

  const glm::uvec3 ptSize = image.pageTableCacheSize();
  if (resources.pageTableCacheSize != ptSize) {
    resources.pageTableCache = createUint3DTexture(m_device, ptSize);
    resources.pageTableCacheSize = ptSize;
  }
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
