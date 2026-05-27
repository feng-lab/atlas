#pragma once

#include "z3dgl.h"
#include "zimgpack.h"
#include "z3dblockcache.h"
#include "z3dtexture.h"
#include "z3dvertexbufferobject.h"
#include "zbbox.h"
#include <folly/CancellationToken.h>
#include <set>
#include <functional>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <chrono>
#include <memory>

#if defined(ATLAS_SANITIZE_ADDRESS)
#define ATLAS_CHECK_CACHE
#include <boost/unordered/unordered_flat_set.hpp>
#endif

namespace nim {

class Z3DShaderProgram;

class ZBenchTimer;
class ZVulkanImageBlockUploader;

// coordinates:
// 1. Voxel Coordinate:    [0, dim.x-1] x [0, dim.y-1] x [0, dim.z-1]
//                     in which (0,0,0) is LeftUpFront Corner (LUF)
//                         and dim-1 is RightDownBack Corner (RDB)
// 2. Texture Coordinate:  [0.0, 1.0] x [0.0, 1.0] x [0.0, 1.0]
// might throw exception
class Z3DImg
{
public:
  // Z3DImg builds per-channel textures and caches for rendering
  Z3DImg(const ZImgPack& imgPack,
         const glm::vec3& scale,
         const std::vector<glm::dvec2>& displayRanges);
  ~Z3DImg();

  [[nodiscard]] const ZImgPack& imgPack() const
  {
    return m_imgPack;
  }

  [[nodiscard]] bool is2DData() const
  {
    return m_imgPack.imgInfo().depth == 1;
  }

  [[nodiscard]] bool is3DData() const
  {
    return m_imgPack.imgInfo().depth > 1;
  }

  [[nodiscard]] glm::dvec2 displayRange(size_t ch) const
  {
    return m_channelDisplayRanges.at(ch);
  }

  [[nodiscard]] glm::uvec3 dimensions() const
  {
    return glm::uvec3(m_imgPack.imgInfo().width, m_imgPack.imgInfo().height, m_imgPack.imgInfo().depth);
  }

  [[nodiscard]] size_t numChannels() const
  {
    return m_nChannels;
  }

  [[nodiscard]] col4 channelColor(size_t c) const
  {
    return m_imgPack.imgInfo().channelColors[c];
  }

  [[nodiscard]] bool isVolumeDownsampled() const
  {
    return m_isVolumeDownsampled;
  }

  [[nodiscard]] std::shared_ptr<const ZImg> channelImageShared(size_t c) const;

  [[nodiscard]] Z3DTexture* channelTexture(size_t c) const;

  // Release GL textures owned by this image (channel and paging textures) to free memory
  // when switching to Vulkan backend. Textures will be recreated lazily when needed.
  void releaseGLResources();

  // Explicitly (re)create GL paging textures (page directory, page table cache, image cache)
  // and reset CPU paging arrays to a consistent empty state. Call when switching back to GL.
  void rebuildGLPagingResources();

  [[nodiscard]] glm::uvec3 channelDimensions(size_t c) const;

  uint64_t volumeGeneration(size_t c) const;

  void addDestructionCallback(std::function<void()> callback);

  // Returns a string representation of the sampler type: "sampler2D" for 2D image, "sampler3D" for 3D volume
  [[nodiscard]] std::string samplerType() const;

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  [[nodiscard]] glm::vec3 physicalLUF() const
  {
    return {0, 0, 0};
  }

  [[nodiscard]] glm::vec3 physicalRDB() const
  {
    return glm::max(glm::vec3(1, 1, 1),
                    glm::vec3(m_imgPack.imgInfo().width, m_imgPack.imgInfo().height, m_imgPack.imgInfo().depth));
  }

  [[nodiscard]] glm::vec3 physicalLDF() const
  {
    return {physicalLUF().x, physicalRDB().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalRDF() const
  {
    return {physicalRDB().x, physicalRDB().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalRUF() const
  {
    return {physicalRDB().x, physicalLUF().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalLUB() const
  {
    return {physicalLUF().x, physicalLUF().y, physicalRDB().z};
  }

  [[nodiscard]] glm::vec3 physicalLDB() const
  {
    return {physicalLUF().x, physicalRDB().y, physicalRDB().z};
  }

  [[nodiscard]] glm::vec3 physicalRUB() const
  {
    return {physicalRDB().x, physicalLUF().y, physicalRDB().z};
  }

  [[nodiscard]] ZBBox<glm::dvec3> physicalBoundBox() const
  {
    return ZBBox<glm::dvec3>(glm::dvec3(physicalLUF()), glm::dvec3(physicalRDB()));
  }

  void setScale(const glm::vec3& scale);

  void setChannelDisplayRanges(const std::vector<glm::dvec2>& displayRanges);

  [[nodiscard]] size_t numLevels() const
  {
    return m_numLevels;
  }

  [[nodiscard]] size_t numCachedImages(size_t c) const
  {
    return m_channelImageCacheManagers[c]->size();
  }

  [[nodiscard]] glm::uvec3 pageDirectorySize() const
  {
    return m_pageDirectorySize;
  }

  [[nodiscard]] glm::uvec3 pageTableCacheSize() const
  {
    return m_pageTableCacheSize;
  }

  [[nodiscard]] glm::uvec3 imageCacheSize() const;

  [[nodiscard]] glm::uvec3 imageBlockSize() const
  {
    return m_imageBlockSize;
  }

  [[nodiscard]] glm::uvec3 imageBlockPadding() const
  {
    return m_imageBlockSizePad;
  }

  [[nodiscard]] glm::uvec3 imageBlockExtent() const
  {
    return m_imageBlockSize + m_imageBlockSizePad;
  }

  [[nodiscard]] size_t imageBlockByteSize() const;

  [[nodiscard]] const glm::uvec4& emptyPageTableEntry() const
  {
    return m_emptyPageTableEntry;
  }

  [[nodiscard]] const std::vector<glm::uvec3>& pageDirectoryBases() const
  {
    return m_pageDirectoryBases;
  }

  [[nodiscard]] const glm::uvec3& pageTableBlockSize() const
  {
    return m_pageTableBlockSize;
  }

  [[nodiscard]] const std::vector<glm::uvec3>& imageDimensionsLevels() const
  {
    return m_imageDimensions;
  }

  [[nodiscard]] const std::vector<float>& voxelWorldSizesLevels() const
  {
    return m_voxelWorldSizes;
  }

  [[nodiscard]] const std::vector<glm::uvec3>& posToBlockIDsLevels() const
  {
    return m_posToBlockIDs;
  }

  [[nodiscard]] uint32_t maxPagedBlockID() const;

  [[nodiscard]] glm::vec3 imageAddressToNormalizedTextureCoord(size_t channel) const
  {
    (void)channel;
    const glm::uvec3 dimension = imageCacheSize();
    CHECK(dimension.x > 0u && dimension.y > 0u && dimension.z > 0u)
      << "dimension=(" << dimension.x << "," << dimension.y << "," << dimension.z << ")";
    return 1.f / glm::vec3(dimension);
  }

  [[nodiscard]] std::span<const glm::uvec4> pageDirectoryView(size_t c) const
  {
    return std::span<const glm::uvec4>(m_channelPageDirectories[c].data(), m_channelPageDirectories[c].size());
  }

  [[nodiscard]] std::span<const glm::uvec4> pageTableCacheView(size_t c) const
  {
    return std::span<const glm::uvec4>(m_channelPageTableCaches[c].data(), m_channelPageTableCaches[c].size());
  }

  void attachVulkanImageBlockUploader(ZVulkanImageBlockUploader* uploader)
  {
    setVulkanImageBlockUploader(uploader);
  }

  void mapImageBlockToCache(size_t channel,
                            const glm::uvec4& pageTableEntryKey,
                            glm::uvec4& pageTableEntryRef)
  {
    insertImageBlockToCache(channel, pageTableEntryKey, pageTableEntryRef);
  }

  void invalidateVulkanPagedImageCache(size_t c);

  template<typename QueueType>
  folly::coro::Task<void>
  readImageBlockToQueueAsync(size_t c,
                             const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                             size_t taskIdx,
                             const ZImgInfo& resInfo,
                             QueueType& queue) const;

  template<typename QueueType>
  folly::coro::Task<void>
  readImageBlocksToQueueAsync(size_t c,
                              const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                              const ZImgInfo& resInfo,
                              QueueType& queue,
                              ZBenchTimer& bt) const;

  void bindFullResBlockIDsShader(Z3DShaderProgram& shader, size_t c) const;

  void bindFullResRenderShader(Z3DShaderProgram& shader, size_t c) const;

  bool updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                          size_t c,
                                          const folly::CancellationToken& cancellationToken,
                                          ZBenchTimer& bt,
                                          uint32_t roundIndex = 0);

  // Optional per-frame/paging telemetry sink (typically installed by the 3D renderer).
  // When null, paging read operations are uninstrumented.
  void setReadStatsSink(/*nullable*/ ZImgReadStatsSink* sink)
  {
    m_readStatsSink = sink;
  }

  [[nodiscard]] ZImgReadStatsSink* readStatsSink() const
  {
    return m_readStatsSink;
  }

  void clearPendingPagingWarnings();
  void recordPagingFailure(const glm::uvec4& pageTableEntryKey, std::string errorMessage);
  [[nodiscard]] std::optional<std::string> takePendingPagingWarning();
  [[nodiscard]] std::optional<std::string> takePendingPreviewWarning();

protected:
  void readVolumes();

  __forceinline bool isImageBlockEmpty(size_t c, const glm::uvec4& pageTableEntryKey, const glm::uvec3& imageBlockSize)
  {
    glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
    return m_imgPack.isEmptyBlock(m_levelScales[blockImagePos.w].x,
                                  m_levelScales[blockImagePos.w].z,
                                  index_t(blockImagePos.x) - index_t(m_imageBlockSizePad.x) / 2,
                                  index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.y) / 2,
                                  index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.z) / 2,
                                  c,
                                  0,
                                  imageBlockSize.x,
                                  imageBlockSize.y,
                                  imageBlockSize.z,
                                  m_channelDisplayRanges[c].x);
  }

  void
  insertPageTableBlockToCache(size_t c, const glm::uvec4& pageDirectoryEntryKey, glm::uvec4& pageDirectoryEntryRef);

  void insertImageBlockToCache(size_t c, const glm::uvec4& pageTableEntryKey, glm::uvec4& pageTableEntryRef);

  folly::coro::Task<void>
  readImageBlockToBufferAsync(size_t c,
                              const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                              size_t taskIdx,
                              const ZImgInfo& resInfo,
                              uint8_t* buffer,
                              std::optional<std::string>* failureMessage) const;

  folly::coro::Task<void>
  readImageBlocksToBufferAsync(size_t c,
                               const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                               const ZImgInfo& resInfo,
                               uint8_t* buffer,
                               std::vector<std::optional<std::string>>& failureMessages) const;

  // return number of empty (all zero) image blocks
  size_t readAndUploadImageBlocks(size_t c,
                                  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                  const folly::CancellationToken& cancellationToken,
                                  ZBenchTimer& bt);

  void setVulkanImageBlockUploader(ZVulkanImageBlockUploader* uploader);

  void checkPageSystemError(size_t c, bool strict = true);

  void resetCacheSystem(size_t c);

  void ensureGLPagingTexturesForChannel(size_t c);

protected:
  const glm::uvec3 m_pageTableBlockSize = glm::uvec3(32, 32, 32);
  glm::uvec3 m_pageTableCacheNumBlocks;
  // glm::uvec3 m_imageBlockSize = glm::uvec3(60, 60, 60);
  glm::uvec3 m_imageBlockSize;
  const glm::uvec3 m_imageBlockSizePad = glm::uvec3(4, 4, 4);
  // glm::ivec3 m_imageBlockReadSize;
  glm::uvec3 m_imageCacheNumBlocks;
  // const uint32_t m_unmappedFlag = 0;
  const uint32_t m_emptyFlag = 40000;
  // for page directory entry, 0 means page table block is not mapped,
  // 1 means page table is mapped but no image blocks are mapped, 40000 means page directory is empty, where
  // all regions in this page directory (can be up to m_pageTableBlockSize * imageBlockSize) contain no signal,
  // value 2 - 1 + 32*32*32(32768) means (1-32768) number of image blocks are mapped
  // for page table entry, 0 means image block is not mapped,
  // 1 means image block is mapped, 40000 means image block is empty (contains no signal)

  std::vector<std::vector<glm::uvec4>> m_channelPageDirectories;
  glm::uvec3 m_pageDirectorySize;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelPageDirectoryTextures;
  std::vector<std::vector<glm::uvec4>> m_channelPageTableCaches;
  glm::uvec3 m_pageTableCacheSize;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelPageTableCacheTextures;
  std::vector<std::unique_ptr<Z3DBlockCache<glm::uvec4>>> m_channelPageTableCacheManagers;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelImageCacheTextures;
  std::vector<std::unique_ptr<Z3DBlockCache<glm::uvec4>>> m_channelImageCacheManagers;
  std::vector<uint8_t> m_glPagingTexturesDirty;
  std::vector<uint64_t> m_volumeGenerations;

  size_t m_numLevels = 1;
  std::vector<glm::uvec3> m_pageDirectoryBases;
  std::vector<glm::uvec3> m_pageDirectoryDimensions;
  std::vector<glm::uvec3> m_pageTableDimensions;
  std::vector<glm::uvec3> m_imageDimensions;
  std::vector<glm::uvec3> m_levelScales;
  std::vector<glm::uvec3> m_posToBlockIDs;
  uint32_t m_maxPagedBlockID = 0;

  std::vector<glm::vec3> m_voxelWorldDimensions;
  std::vector<float> m_voxelWorldSizes;
  glm::vec3 m_volumeVoxelWorldDimension;
  float m_volumeVoxelWorldSize;

private:
  // std::unique_ptr<Z3DImgHistogramThread> m_histogramThread;
  const ZImgPack& m_imgPack;
  struct ChannelResources
  {
    std::shared_ptr<const ZImg> image;
    std::unique_ptr<Z3DTexture> texture;
    uint64_t textureGeneration = 0;
    glm::uvec3 dimensions{0u, 0u, 0u};
  };

  std::vector<ChannelResources> m_channelResources;
  size_t m_nChannels = 0;
  bool m_isVolumeDownsampled;

  std::unique_ptr<Z3DVertexBufferObject> m_PBO;

  std::vector<glm::dvec2> m_channelDisplayRanges;

  size_t m_blockUploadingBatchSize = 100;

  // Preview-volume scales used by readVolumes(). These may be smaller than the GPU-fit
  // threshold when paging is active so the interactive preview stays cheap to build.
  double m_widthScale = 1.0;
  double m_heightScale = 1.0;
  double m_depthScale = 1.0;
  glm::uvec3 m_volumeDimension;
  glm::vec3 m_volumeSpacing;

  std::vector<std::function<void()>> m_destructionCallbacks;

#ifdef ATLAS_CHECK_CACHE
  boost::unordered_flat_set<glm::uvec3> m_usedPageTableEntry;
#endif

  const glm::uvec4 m_invalidKey = glm::uvec4(std::numeric_limits<uint32_t>::max());
  const glm::uvec4 m_emptyPageTableEntry = glm::uvec4(std::numeric_limits<uint32_t>::max(),
                                                      std::numeric_limits<uint32_t>::max(),
                                                      std::numeric_limits<uint32_t>::max(),
                                                      m_emptyFlag);

  size_t m_maxMemoryForPageTableCache;
  size_t m_maxMemoryForImageCache;
  bool m_hasSufficientPageTableCacheSpace = false;

  ZVulkanImageBlockUploader* m_vulkanImageBlockUploader = nullptr;

  // Rate-limited logging: when enabled via VLOG, summarize paging demand per LOD.
  std::vector<std::chrono::steady_clock::time_point> m_lastPagingLodStatsLogTimes;

  // Optional paging read stats sink and per-call round tag.
  ZImgReadStatsSink* m_readStatsSink = nullptr;
  uint32_t m_activeReadStatsRound = 0;
  std::vector<std::string> m_pendingPagingWarnings;
  std::optional<std::string> m_pendingPreviewWarning;
};

} // namespace nim
