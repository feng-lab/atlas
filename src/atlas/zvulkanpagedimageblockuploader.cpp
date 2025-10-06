#include "zvulkanpagedimageblockuploader.h"

#include "z3dimg.h"
#include "zbenchtimer.h"
#include "zcancellation.h"
#include "zlog.h"
#include "zvulkandevice.h"
#include "zvulkantexture.h"

#include <folly/MPMCQueue.h>
#include <folly/coro/FutureUtil.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>

#include <chrono>

DECLARE_uint32(atlas_log_folly_global_executor_status_interval_in_seconds);

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
  sampler.unnormalizedCoordinates = VK_FALSE;
  sampler.borderColor = vk::BorderColor::eIntOpaqueBlack;
  sampler.compareEnable = VK_FALSE;
  sampler.anisotropyEnable = VK_FALSE;
  sampler.maxAnisotropy = 1.0f;
  sampler.minLod = 0.0f;
  sampler.maxLod = 0.0f;
  return sampler;
}

vk::SamplerCreateInfo makeLinearSampler()
{
  vk::SamplerCreateInfo sampler{};
  sampler.magFilter = vk::Filter::eLinear;
  sampler.minFilter = vk::Filter::eLinear;
  sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
  sampler.addressModeU = vk::SamplerAddressMode::eClampToBorder;
  sampler.addressModeV = vk::SamplerAddressMode::eClampToBorder;
  sampler.addressModeW = vk::SamplerAddressMode::eClampToBorder;
  sampler.unnormalizedCoordinates = VK_FALSE;
  sampler.borderColor = vk::BorderColor::eFloatTransparentBlack;
  sampler.compareEnable = VK_FALSE;
  sampler.anisotropyEnable = VK_FALSE;
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

  auto info = ZVulkanTexture::CreateInfo::make3D(size.x,
                                                 size.y,
                                                 size.z,
                                                 vk::Format::eR32G32B32A32Uint,
                                                 vk::ImageUsageFlagBits::eSampled |
                                                   vk::ImageUsageFlagBits::eTransferDst,
                                                 vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                 1u,
                                                 false,
                                                 vk::ImageLayout::eShaderReadOnlyOptimal);
  info.createDefaultSampler = false;
  info.samplerInfo = makeNearestSampler();
  return std::make_unique<ZVulkanTexture>(device, info);
}

std::unique_ptr<ZVulkanTexture> createImageCacheTexture(ZVulkanDevice& device, glm::uvec3 size)
{
  if (size.x == 0u || size.y == 0u || size.z == 0u) {
    return nullptr;
  }

  auto info = ZVulkanTexture::CreateInfo::make3D(size.x,
                                                 size.y,
                                                 size.z,
                                                 vk::Format::eR8Unorm,
                                                 vk::ImageUsageFlagBits::eSampled |
                                                   vk::ImageUsageFlagBits::eTransferDst,
                                                 vk::MemoryPropertyFlagBits::eDeviceLocal,
                                                 1u,
                                                 false,
                                                 vk::ImageLayout::eShaderReadOnlyOptimal);
  info.createDefaultSampler = false;
  info.samplerInfo = makeLinearSampler();
  return std::make_unique<ZVulkanTexture>(device, info);
}

} // namespace

ZVulkanPagedImageBlockUploader::ZVulkanPagedImageBlockUploader(ZVulkanDevice& device)
  : m_device(device)
  , m_aliveFlag(std::make_shared<bool>(true))
{}

ZVulkanPagedImageBlockUploader::~ZVulkanPagedImageBlockUploader()
{
  *m_aliveFlag = false;
  std::scoped_lock lock(m_mutex);
  m_resources.clear();
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

  ensureImageResources(image);

  ZVulkanTexture* imageCacheTexture = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    const auto* resources = findImageResourcesLocked(image);
    CHECK(resources != nullptr);
    CHECK_LT(channel, resources->channels.size());
    imageCacheTexture = resources->channels[channel].imageCache.get();
  }

  if (imageCacheTexture == nullptr) {
    LOG(WARNING) << "Vulkan image cache texture missing for channel " << channel;
    return 0;
  }

  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto* threadPool = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get());
  CHECK(threadPool != nullptr);

  const glm::uvec3 extent = image.imageBlockExtent();
  const size_t bytesPerBlock = image.imageBlockByteSize();
  const ZImgInfo resInfo(extent.x, extent.y, extent.z, 1);

  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<ZImg>>, true> imgQueue;
  [[maybe_unused]] auto readFuture = folly::coro::toFuture(
    folly::coro::co_withCancellation(cancellationToken,
                                     image.readImageBlocksToQueueAsync(channel, pendingTasks, resInfo, imgQueue, timer)),
    cpuExecutor);

  size_t emptyBlockCount = 0;
  int remainingBlocks = static_cast<int>(pendingTasks.size());
  std::tuple<size_t, std::shared_ptr<ZImg>> elem;
  auto lastLog = std::chrono::steady_clock::now();

  while (remainingBlocks > 0) {
    processEventsAndMaybeCancel(cancellationToken);

    if (imgQueue.try_dequeue_until(
          elem,
          std::chrono::steady_clock::now() +
            std::chrono::seconds(FLAGS_atlas_log_folly_global_executor_status_interval_in_seconds))) {
      const auto taskIndex = std::get<0>(elem);
      const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[taskIndex];

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

        imageCacheTexture->uploadSubImage(std::get<1>(elem)->channelData(0), bytesPerBlock, uploadRegion);
      }

      --remainingBlocks;
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

  timer.recordEvent("image blocks uploading");
  return emptyBlockCount;
}

void ZVulkanPagedImageBlockUploader::uploadPageCaches(Z3DImg& image, size_t channel, ZBenchTimer& timer)
{
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

  auto pageDirectorySpan = image.pageDirectoryView(channel);
  if (pageDirectory && !pageDirectorySpan.empty()) {
    pageDirectory->uploadData(pageDirectorySpan.data(), pageDirectorySpan.size_bytes());
  }

  auto pageTableSpan = image.pageTableCacheView(channel);
  if (pageTableCache && !pageTableSpan.empty()) {
    pageTableCache->uploadData(pageTableSpan.data(), pageTableSpan.size_bytes());
  }

  timer.recordEvent("upload page table");
}

ZVulkanTexture*
ZVulkanPagedImageBlockUploader::pageDirectoryTexture(const Z3DImg& image, size_t channel) const
{
  std::scoped_lock lock(m_mutex);
  const auto* resources = findImageResourcesLocked(image);
  if (!resources || channel >= resources->channels.size()) {
    return nullptr;
  }
  return resources->channels[channel].pageDirectory.get();
}

ZVulkanTexture*
ZVulkanPagedImageBlockUploader::pageTableTexture(const Z3DImg& image, size_t channel) const
{
  std::scoped_lock lock(m_mutex);
  const auto* resources = findImageResourcesLocked(image);
  if (!resources || channel >= resources->channels.size()) {
    return nullptr;
  }
  return resources->channels[channel].pageTableCache.get();
}

ZVulkanTexture*
ZVulkanPagedImageBlockUploader::imageCacheTexture(const Z3DImg& image, size_t channel) const
{
  std::scoped_lock lock(m_mutex);
  const auto* resources = findImageResourcesLocked(image);
  if (!resources || channel >= resources->channels.size()) {
    return nullptr;
  }
  return resources->channels[channel].imageCache.get();
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

  const glm::uvec3 cacheSize = image.imageCacheSize();
  if (resources.imageCacheSize != cacheSize) {
    resources.imageCache = createImageCacheTexture(m_device, cacheSize);
    resources.imageCacheSize = cacheSize;
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
    std::weak_ptr<bool> alive = m_aliveFlag;
    ZVulkanPagedImageBlockUploader* self = this;
    image.addDestructionCallback([alive, self, key = &image]() {
      auto alivePtr = alive.lock();
      if (!alivePtr || !*alivePtr) {
        return;
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
