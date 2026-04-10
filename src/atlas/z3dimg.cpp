#include "z3dimg.h"

#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3drenderglobalstate.h"
#include "z3dtexture.h"
#include "zvulkanimageblockuploader.h"
#include "zkmeans.h"
#include "zbenchtimer.h"
#include "zcancellation.h"
#include "zcpuinfo.h"
#include "zneuroglancerprecomputed.h"
#include <folly/OperationCancelled.h>
#include <folly/coro/Collect.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/MPMCQueue.h>
#include <folly/executors/ThreadPoolExecutor.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/FutureUtil.h>
#include <boost/unordered/unordered_flat_set.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>

DEFINE_uint32(atlas_log_folly_global_executor_status_interval_in_seconds,
              5,
              "Interval in seconds for logging folly global executor status during waiting, default is 5");

DEFINE_uint32(atlas_3d_paging_queue_poll_interval_ms,
              50,
              "Polling interval in milliseconds while waiting for 3D paging block reads (lower improves cancellation responsiveness), default is 50ms");

DEFINE_uint32(atlas_ng_precomputed_3d_max_concurrent_block_reads,
              256,
              "Maximum number of concurrent Neuroglancer block read tasks during 3D paging (limits network pressure and speeds up cancellation), default is 256");

DEFINE_uint32(atlas_3d_paging_lod_stats_log_interval_ms,
              500,
              "Minimum interval in milliseconds between rate-limited per-LOD paging statistics logs, default is 500ms");

DEFINE_uint32(atlas_number_of_blocks_to_use_PBO_threashold,
              0,
              "Use PBO when number of blocks to upload is larger than this threshold, default is 0");

DEFINE_uint32(atlas_3d_preview_max_dimension,
              512,
              "Maximum per-axis dimension of the preloaded 3D preview volume when paging is active. "
              "This does not affect whether a volume is considered GPU-fit; that decision still uses "
              "getDataScaleForTexture().");

DEFINE_uint32(
  atlas_image_block_size,
  64,
  "define the width, height, and depth of image blocks in the 3D image cache system, can be 64 (default), 128, 256 or 512");

DECLARE_double(atlas_image_cache_memory_proportion);
DECLARE_double(atlas_image_region_cache_memory_proportion);

namespace nim {

namespace {

std::unique_ptr<Z3DTexture> createChannelTexture(const ZImg& image)
{
  glm::uvec3 dimensions(image.width(), image.height(), image.depth());

  GLenum format = GL_RED;
  GLint internalFormat = GLint(GL_R8);
  GLenum dataType = GL_UNSIGNED_BYTE;

  if (image.isType<uint8_t>()) {
    internalFormat = GLint(GL_R8);
    dataType = GL_UNSIGNED_BYTE;
  } else if (image.isType<uint16_t>()) {
    internalFormat = GLint(GL_R16);
    dataType = GL_UNSIGNED_SHORT;
  } else if (image.isType<float>()) {
    internalFormat = GLint(GL_R32F);
    dataType = GL_FLOAT;
  } else {
    LOG(ERROR) << "Unsupported volume voxel format for channel texture.";
    return nullptr;
  }

  // Volume previews should clamp to the dataset edge rather than sample beyond the
  // texture domain, which avoids driver-dependent edge interpolation artifacts.
  return std::make_unique<Z3DTexture>(internalFormat,
                                      dimensions,
                                      format,
                                      dataType,
                                      image.channelData(0),
                                      GLint(GL_LINEAR),
                                      GLint(GL_LINEAR),
                                      GLint(GL_CLAMP_TO_EDGE));
}

std::array<size_t, 3> bestRatioAtMost(const std::vector<std::array<size_t, 3>>& candidates,
                                      const std::array<size_t, 3>& requested)
{
  std::array<size_t, 3> best = {1, 1, 1};
  size_t bestSum = 3;
  for (const auto& ratio : candidates) {
    if (ratio[0] <= requested[0] && ratio[1] <= requested[1] && ratio[2] <= requested[2]) {
      const size_t sum = ratio[0] + ratio[1] + ratio[2];
      if (sum > bestSum) {
        best = ratio;
        bestSum = sum;
      }
    }
  }
  return best;
}

double cappedAxisScale(size_t dim, uint32_t maxDim)
{
  CHECK_GT(maxDim, 0u) << "atlas_3d_preview_max_dimension must be > 0";
  return dim <= maxDim ? 1.0 : static_cast<double>(maxDim) / static_cast<double>(dim);
}

} // namespace

Z3DImg::Z3DImg(const ZImgPack& imgPack, const glm::vec3& scale, const std::vector<glm::dvec2>& displayRanges)
  : m_imgPack(imgPack)
  , m_channelDisplayRanges(displayRanges)
{
  for (const auto& dr : m_channelDisplayRanges) {
    VLOG(1) << dr;
  }
  const ZImgInfo& info = m_imgPack.imgInfo();
  double fitWidthScale = 1.0;
  double fitHeightScale = 1.0;
  double fitDepthScale = 1.0;
  // Keep the resident-vs-paged decision tied to actual GPU-fit limits, not the preview heuristic.
  Z3DGpuInfo::instance()
    .getDataScaleForTexture(info.width, info.height, info.depth, fitWidthScale, fitHeightScale, fitDepthScale);
  m_isVolumeDownsampled = fitWidthScale != 1.0 || fitHeightScale != 1.0 || fitDepthScale != 1.0;

  m_widthScale = fitWidthScale;
  m_heightScale = fitHeightScale;
  m_depthScale = fitDepthScale;
  if (m_isVolumeDownsampled && info.depth > 1) {
    // For paged 3D data, keep the preview policy separate from the resident-fit decision.
    // The fit scales answer "does the full volume fit the GPU natively?".
    // The preview scales answer "how large should the fast preview volume be?".
    // Reusing fitScale for preview can make extreme-aspect volumes unnecessarily tiny.
    m_widthScale = cappedAxisScale(info.width, FLAGS_atlas_3d_preview_max_dimension);
    m_heightScale = cappedAxisScale(info.height, FLAGS_atlas_3d_preview_max_dimension);
    m_depthScale = cappedAxisScale(info.depth, FLAGS_atlas_3d_preview_max_dimension);
  }

  VLOG(1) << "3D image fit scales: (" << fitWidthScale << ", " << fitHeightScale << ", " << fitDepthScale
          << "), preview scales: (" << m_widthScale << ", " << m_heightScale << ", " << m_depthScale
          << "), paged=" << m_isVolumeDownsampled;

  m_volumeDimension = glm::uvec3(info.width * m_widthScale, info.height * m_heightScale, info.depth * m_depthScale);
  m_volumeSpacing = glm::vec3(1.f / m_widthScale, 1.f / m_heightScale, 1.f / m_depthScale);

  m_nChannels = m_imgPack.imgInfo().numChannels;
  m_volumeGenerations.assign(m_nChannels, 0);
  m_PBO.reset();
  m_lastPagingLodStatsLogTimes.assign(m_nChannels, std::chrono::steady_clock::time_point{});

#if 0
  // shader limit is 20 channels
  // limited by Max FS Texture Image Units
  // see https://www.opengl.org/wiki/Shader#Resource_limitations
  size_t maxPossibleChannels = std::min(20, (Z3DGpuInfoInstance.maxTextureImageUnits() - 4) / 2);
#else
  size_t maxPossibleChannels = Z3DGpuInfo::instance().maxArrayTextureLayers();
#endif
  if (m_nChannels > maxPossibleChannels) {
    QString errMsg =
      QString("Due to hardware limit, only first %1 channels of this image will be shown").arg(maxPossibleChannels);
    LOG(WARNING) << errMsg;
    m_nChannels = maxPossibleChannels;
  }

  if (m_isVolumeDownsampled) {
    if (FLAGS_atlas_image_block_size == 64 || FLAGS_atlas_image_block_size == 128 ||
        FLAGS_atlas_image_block_size == 256 || FLAGS_atlas_image_block_size == 512) {
      m_imageBlockSize = glm::uvec3(FLAGS_atlas_image_block_size) - m_imageBlockSizePad;
    } else {
      constexpr uint32_t defaultImageBlockSize = 64;
      LOG(INFO) << fmt::format("atlas_image_block_size {} is not supported, use {}",
                               FLAGS_atlas_image_block_size,
                               defaultImageBlockSize);
      m_imageBlockSize = glm::uvec3(defaultImageBlockSize) - m_imageBlockSizePad;
    }

    if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 32000) {
#ifdef Q_OS_MACOS
      m_maxMemoryForImageCache = 8_uz * 1024 * 1024 * 1024; // 8G
#else
      m_maxMemoryForImageCache = 8_uz * 1024 * 1024 * 1024; // 8G
#endif
      m_maxMemoryForPageTableCache = 1024_uz * 1024 * 1024; // 1G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 20000) {
      m_maxMemoryForImageCache = 8_uz * 1024 * 1024 * 1024; // 8G
      m_maxMemoryForPageTableCache = 1024_uz * 1024 * 1024; // 1G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 16000) {
      m_maxMemoryForImageCache = 6_uz * 1024 * 1024 * 1024; // 6G
      m_maxMemoryForPageTableCache = 512_uz * 1024 * 1024; // 0.5G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 12000) {
      m_maxMemoryForImageCache = 4_uz * 1024 * 1024 * 1024; // 4G
      m_maxMemoryForPageTableCache = 512_uz * 1024 * 1024; // 0.5G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 8000) {
      m_maxMemoryForImageCache = 2_uz * 1024 * 1024 * 1024; // 2G
      m_maxMemoryForPageTableCache = 512_uz * 1024 * 1024; // 0.5G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 4000) {
      m_maxMemoryForImageCache = 1024_uz * 1024 * 1024; // 1G
      m_maxMemoryForPageTableCache = 256_uz * 1024 * 1024; // 0.25G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 2000) {
      m_maxMemoryForImageCache = 512_uz * 1024 * 1024; // 0.5G
      m_maxMemoryForPageTableCache = 128_uz * 1024 * 1024; // 0.125G
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 1000) {
      m_maxMemoryForImageCache = 256_uz * 1024 * 1024; // 0.25G
      m_maxMemoryForPageTableCache = 64_uz * 1024 * 1024; // 64MB
    } else {
      m_maxMemoryForImageCache = 128_uz * 1024 * 1024; // 0.125G
      m_maxMemoryForPageTableCache = 32_uz * 1024 * 1024; // 32MB
    }

    setScale(scale);
    // setScale(glm::vec3(1,1,5));

    auto calculatedBlockUploadingBatchSize = std::round(ZCpuInfo::instance().nPhysicalRAM *
                                                        (1.0 - 0.1 - FLAGS_atlas_image_cache_memory_proportion -
                                                         FLAGS_atlas_image_region_cache_memory_proportion) /
                                                        (1.9 * 1024 * 1024 * 1024)) * // (2.56 * 1024 * 1024 * 1024))
                                             100.;
    m_blockUploadingBatchSize = static_cast<size_t>(std::clamp(calculatedBlockUploadingBatchSize, 100., 32768.));
    LOG(INFO) << fmt::format("use block uploading batch size: {}", m_blockUploadingBatchSize);
  }
}

Z3DImg::~Z3DImg()
{
  for (auto& callback : m_destructionCallbacks) {
    if (callback) {
      callback();
    }
  }
}

std::string Z3DImg::samplerType() const
{
  if (is3DData()) {
    return "sampler3D";
  }

  return "sampler2D";
}

std::shared_ptr<const ZImg> Z3DImg::channelImageShared(size_t c) const
{
  CHECK_LT(c, m_channelResources.size());
  CHECK(m_channelResources[c].image != nullptr);
  return m_channelResources[c].image;
}

Z3DTexture* Z3DImg::channelTexture(size_t c) const
{
  CHECK_LT(c, m_channelResources.size());
  auto& channel = const_cast<Z3DImg*>(this)->m_channelResources[c];
  CHECK(channel.image != nullptr);
  const uint64_t generation = c < m_volumeGenerations.size() ? m_volumeGenerations[c] : 0;
  if (!channel.texture || channel.textureGeneration != generation) {
    channel.texture = createChannelTexture(*channel.image);
    CHECK(channel.texture != nullptr) << "Failed to create channel texture.";
    channel.textureGeneration = generation;
  }
  return channel.texture.get();
}

glm::uvec3 Z3DImg::channelDimensions(size_t c) const
{
  CHECK_LT(c, m_channelResources.size());
  return m_channelResources[c].dimensions;
}

uint64_t Z3DImg::volumeGeneration(size_t c) const
{
  if (c < m_volumeGenerations.size()) {
    return m_volumeGenerations[c];
  }
  return 0;
}

void Z3DImg::addDestructionCallback(std::function<void()> callback)
{
  m_destructionCallbacks.emplace_back(std::move(callback));
}

glm::uvec3 Z3DImg::imageCacheSize() const
{
  const glm::uvec3 blockExtent = m_imageBlockSize + m_imageBlockSizePad;
  return glm::uvec3(m_imageCacheNumBlocks.x * blockExtent.x,
                    m_imageCacheNumBlocks.y * blockExtent.y,
                    m_imageCacheNumBlocks.z * blockExtent.z);
}

size_t Z3DImg::imageBlockByteSize() const
{
  const glm::uvec3 extent = imageBlockExtent();
  const ZImgInfo info(extent.x, extent.y, extent.z, 1);
  return info.byteNumber();
}

void Z3DImg::setScale(const glm::vec3& scale)
{
  if (!m_isVolumeDownsampled) {
    return;
  }

  const ZImgInfo& info = m_imgPack.imgInfo();

  // Invariant: the paging LOD hierarchy should be stable for datasets that provide
  // physical voxel spacing. Many pyramidal formats use anisotropic steps (e.g. 2x2x1)
  // to respect voxel sizes; tying LOD generation to a user-controlled transform scale
  // can cause ratio mismatches and unnecessary resampling.
  //
  // Therefore:
  // - If voxel size is available, derive LOD anisotropy from voxelSize{X,Y,Z}.
  // - Otherwise (no physical spacing), preserve legacy behavior and derive anisotropy
  //   from the user-provided scale.
  const bool hasPhysicalVoxelSize =
    info.voxelSizeUnit != VoxelSizeUnit::none && std::isfinite(info.voxelSizeX) && std::isfinite(info.voxelSizeY) &&
    std::isfinite(info.voxelSizeZ) && info.voxelSizeX > 0.0 && info.voxelSizeY > 0.0 && info.voxelSizeZ > 0.0;

  glm::dvec3 imgDim = glm::dvec3(info.width, info.height, info.depth);
  glm::dvec3 relativeResolution = [&]() -> glm::dvec3 {
    if (hasPhysicalVoxelSize) {
      const double xy = std::max(info.voxelSizeX, info.voxelSizeY);
      CHECK(std::isfinite(xy) && xy > 0.0);
      return glm::dvec3(xy, xy, info.voxelSizeZ);
    }
    return glm::dvec3(glm::abs(scale));
  }();
  // make x and y scales same
  relativeResolution.x = std::max(relativeResolution.x, relativeResolution.y);
  relativeResolution.y = relativeResolution.x;

  double minRes = std::min(std::min(relativeResolution.x, relativeResolution.y), relativeResolution.z);
  relativeResolution /= minRes;
  imgDim *= relativeResolution;
  glm::dvec3 levels = glm::ceil(glm::log2(imgDim / glm::dvec3(m_imageBlockSize))) + 1.0;
  const size_t requestedNumLevels = std::max(std::max(levels.x, levels.y), levels.z);

  std::vector<size_t> sortedIndex = argSort(&relativeResolution[0], &relativeResolution[0] + 3);
  const std::vector<size_t> baseStayRounds = [&]() {
    std::vector<size_t> stayRounds(3, 0);
    CHECK(relativeResolution[sortedIndex[0]] == 1.0);
    for (size_t i = 1; i < 3; ++i) {
      double res = relativeResolution[sortedIndex[0]];
      while (true) {
        if (res * 2 < relativeResolution[sortedIndex[i]]) {
          res *= 2;
          ++stayRounds[sortedIndex[i]];
        } else {
          if ((res * 2 - relativeResolution[sortedIndex[i]]) < (relativeResolution[sortedIndex[i]] - res)) {
            ++stayRounds[sortedIndex[i]];
          }
          break;
        }
      }
    }
    return stayRounds;
  }();

  const auto candidateLevelScales = [&]() {
    std::vector<glm::uvec3> out;
    out.resize(requestedNumLevels);
    std::vector<size_t> stayRounds = baseStayRounds;
    for (size_t l = 0; l < requestedNumLevels; ++l) {
      if (l == 0) {
        out[l] = glm::uvec3(1, 1, 1);
      } else {
        if (stayRounds[sortedIndex[2]] > stayRounds[sortedIndex[1]]) {
          --stayRounds[sortedIndex[2]];
          out[l][sortedIndex[2]] = out[l - 1][sortedIndex[2]];
          out[l][sortedIndex[1]] = out[l - 1][sortedIndex[1]] * 2;
          out[l][sortedIndex[0]] = out[l - 1][sortedIndex[0]] * 2;
        } else if (stayRounds[sortedIndex[2]] > 0) {
          CHECK(stayRounds[sortedIndex[2]] == stayRounds[sortedIndex[1]]);
          --stayRounds[sortedIndex[2]];
          --stayRounds[sortedIndex[1]];
          out[l][sortedIndex[2]] = out[l - 1][sortedIndex[2]];
          out[l][sortedIndex[1]] = out[l - 1][sortedIndex[1]];
          out[l][sortedIndex[0]] = out[l - 1][sortedIndex[0]] * 2;
        } else {
          out[l] = out[l - 1] * 2_u32;
        }
      }
      CHECK(out[l].x == out[l].y);
    }
    return out;
  }();

  auto isStructureIdentical = [&](size_t effectiveNumLevels) -> bool {
    if (m_levelScales.empty() || effectiveNumLevels != m_numLevels) {
      return false;
    }
    for (size_t l = 0; l < effectiveNumLevels; ++l) {
      const auto& a = m_levelScales[l];
      const auto& b = candidateLevelScales[l];
      if (a.x != b.x || a.y != b.y || a.z != b.z) {
        return false;
      }
    }
    return true;
  };

  // Determine the effective number of LOD levels after clamping the coarsest
  // level to the preloaded downsampled volume (m_volumeDimension).
  const glm::vec3 absScale = glm::abs(scale);
  const glm::vec3 candidateVolumeVoxelWorldDimension = absScale * m_volumeSpacing;
  const float candidateVolumeVoxelWorldSize = std::max(std::max(candidateVolumeVoxelWorldDimension.x,
                                                                candidateVolumeVoxelWorldDimension.y),
                                                       candidateVolumeVoxelWorldDimension.z);
  size_t candidateEffectiveNumLevels = requestedNumLevels;
  for (size_t l = 0; l < requestedNumLevels; ++l) {
    if (hasPhysicalVoxelSize) {
      // Physical-resolution mode: clamp based purely on voxel-downsample factors, independent of
      // user transform scale. This keeps the paging hierarchy stable.
      const glm::vec3 levelScaleF(candidateLevelScales[l]);
      if (levelScaleF.x >= m_volumeSpacing.x && levelScaleF.y >= m_volumeSpacing.y && levelScaleF.z >= m_volumeSpacing.z) {
        candidateEffectiveNumLevels = l + 1;
        break;
      }
    } else {
      const glm::vec3 voxelWorldDim = absScale * glm::vec3(candidateLevelScales[l]);
      const float voxelWorldSize = std::min(std::min(voxelWorldDim.x, voxelWorldDim.y), voxelWorldDim.z);
      if (voxelWorldSize > candidateVolumeVoxelWorldSize) {
        candidateEffectiveNumLevels = l + 1;
        break;
      }
    }
  }

  // Fast path: if the LOD hierarchy itself doesn't change, keep page tables / caches intact
  // and only update the world-size metadata used by shaders.
  if (isStructureIdentical(candidateEffectiveNumLevels)) {
    if (glm::all(glm::epsilonEqual(candidateVolumeVoxelWorldDimension, m_volumeVoxelWorldDimension, 1e-6f))) {
      return;
    }
    m_volumeVoxelWorldDimension = candidateVolumeVoxelWorldDimension;
    m_volumeVoxelWorldSize = candidateVolumeVoxelWorldSize;
    if (m_voxelWorldDimensions.size() != m_numLevels) {
      m_voxelWorldDimensions.resize(m_numLevels);
    }
    if (m_voxelWorldSizes.size() != m_numLevels) {
      m_voxelWorldSizes.resize(m_numLevels);
    }
    for (size_t l = 0; l < m_numLevels; ++l) {
      m_voxelWorldDimensions[l] = absScale * glm::vec3(m_levelScales[l]);
      m_voxelWorldSizes[l] =
        std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
    }
    return;
  }

  m_numLevels = requestedNumLevels;
  m_volumeVoxelWorldDimension = candidateVolumeVoxelWorldDimension;
  m_volumeVoxelWorldSize = candidateVolumeVoxelWorldSize;
  LOG(INFO) << "volumeDimension: " << m_volumeDimension << " volumeVoxelWorldDimension: " << m_volumeVoxelWorldDimension
            << " volumeVoxelWorldSize: " << m_volumeVoxelWorldSize;

  m_pageDirectorySize = glm::uvec3(0, 0, 0);
  m_levelScales.resize(m_numLevels);
  m_imageDimensions.resize(m_numLevels);
  m_pageTableDimensions.resize(m_numLevels);
  m_pageDirectoryDimensions.resize(m_numLevels);
  m_posToBlockIDs.resize(m_numLevels);
  m_pageDirectoryBases.resize(m_numLevels);
  m_voxelWorldDimensions.resize(m_numLevels);
  m_voxelWorldSizes.resize(m_numLevels);
  size_t numImageBlocks = 0;
  size_t numPageTableBlocks = 0;
  std::vector<size_t> stayRounds = baseStayRounds;
  for (size_t l = 0; l < m_numLevels; ++l) {
    if (l == 0) {
      m_levelScales[l] = glm::uvec3(1, 1, 1);
    } else {
      if (stayRounds[sortedIndex[2]] > stayRounds[sortedIndex[1]]) {
        --stayRounds[sortedIndex[2]];
        m_levelScales[l][sortedIndex[2]] = m_levelScales[l - 1][sortedIndex[2]];
        m_levelScales[l][sortedIndex[1]] = m_levelScales[l - 1][sortedIndex[1]] * 2;
        m_levelScales[l][sortedIndex[0]] = m_levelScales[l - 1][sortedIndex[0]] * 2;
      } else if (stayRounds[sortedIndex[2]] > 0) {
        CHECK(stayRounds[sortedIndex[2]] == stayRounds[sortedIndex[1]]);
        --stayRounds[sortedIndex[2]];
        --stayRounds[sortedIndex[1]];
        m_levelScales[l][sortedIndex[2]] = m_levelScales[l - 1][sortedIndex[2]];
        m_levelScales[l][sortedIndex[1]] = m_levelScales[l - 1][sortedIndex[1]];
        m_levelScales[l][sortedIndex[0]] = m_levelScales[l - 1][sortedIndex[0]] * 2;
      } else {
        m_levelScales[l] = m_levelScales[l - 1] * 2_u32;
      }
    }
    CHECK(m_levelScales[l].x == m_levelScales[l].y);

    m_voxelWorldDimensions[l] = absScale * glm::vec3(m_levelScales[l]);
    m_voxelWorldSizes[l] =
      std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
    bool clampToPreview = false;
    if (hasPhysicalVoxelSize) {
      // In physical-resolution mode, keep the hierarchy stable by clamping once
      // this level is coarser than the preloaded downsampled volume in all axes.
      const glm::vec3 levelScaleF(m_levelScales[l]);
      clampToPreview =
        levelScaleF.x >= m_volumeSpacing.x && levelScaleF.y >= m_volumeSpacing.y && levelScaleF.z >= m_volumeSpacing.z;
    } else {
      clampToPreview = m_voxelWorldSizes[l] > m_volumeVoxelWorldSize;
    }
    if (clampToPreview) {
      m_numLevels = l + 1;
      m_imageDimensions[l] = m_volumeDimension;
      m_voxelWorldDimensions[l] = m_volumeVoxelWorldDimension;
      m_voxelWorldSizes[l] = m_volumeVoxelWorldSize;
      m_pageTableDimensions[l] = glm::uvec3(0);
      m_pageDirectoryDimensions[l] = glm::uvec3(0);
      m_posToBlockIDs[l] = glm::uvec3(1 + numImageBlocks, 0, 0);
      m_pageDirectoryBases[l] = glm::uvec3(std::numeric_limits<uint32_t>::max());
      break;
    }

    m_imageDimensions[l] = glm::uvec3((info.width + m_levelScales[l].x - 1) / m_levelScales[l].x,
                                      (info.height + m_levelScales[l].y - 1) / m_levelScales[l].y,
                                      (info.depth + m_levelScales[l].z - 1) / m_levelScales[l].z);
    m_pageTableDimensions[l] = (m_imageDimensions[l] + m_imageBlockSize - 1_u32) / m_imageBlockSize;
    m_pageDirectoryDimensions[l] = (m_pageTableDimensions[l] + m_pageTableBlockSize - 1_u32) / m_pageTableBlockSize;

    numImageBlocks += size_t(m_pageTableDimensions[l].x) * m_pageTableDimensions[l].y * m_pageTableDimensions[l].z;
    numPageTableBlocks +=
      size_t(m_pageDirectoryDimensions[l].x) * m_pageDirectoryDimensions[l].y * m_pageDirectoryDimensions[l].z;

    // id starts from 1
    m_posToBlockIDs[l] =
      glm::uvec3(l == 0 ? 1
                        : (m_posToBlockIDs[l - 1].x + m_pageTableDimensions[l - 1].x * m_pageTableDimensions[l - 1].y *
                                                        m_pageTableDimensions[l - 1].z),
                 m_pageTableDimensions[l].x,
                 m_pageTableDimensions[l].x * m_pageTableDimensions[l].y);
    if (l == 0) {
      m_pageDirectoryBases[l] = glm::uvec3(0, 0, 0);
    } else if (l == 1) {
      m_pageDirectoryBases[l] = m_pageDirectoryBases[l - 1];
      m_pageDirectoryBases[l][sortedIndex[1]] += m_pageDirectoryDimensions[l - 1][sortedIndex[1]];
    } else {
      m_pageDirectoryBases[l] = m_pageDirectoryBases[l - 1];
      m_pageDirectoryBases[l][sortedIndex[0]] += m_pageDirectoryDimensions[l - 1][sortedIndex[0]];
    }
    m_pageDirectorySize = glm::max(m_pageDirectorySize, m_pageDirectoryBases[l] + m_pageDirectoryDimensions[l]);
    if (m_pageDirectorySize.x > Z3DGpuInfo::instance().max3DTextureSize() ||
        m_pageDirectorySize.y > Z3DGpuInfo::instance().max3DTextureSize() ||
        m_pageDirectorySize.z > Z3DGpuInfo::instance().max3DTextureSize()) {
      throw ZException(fmt::format("Image ({}) is not supported", info));
    }
  }

  LOG(INFO) << "pageDirectorySize: " << m_pageDirectorySize << " numPageTableBlocks: " << numPageTableBlocks
            << " numImageBlocks: " << numImageBlocks;

  m_pageTableCacheNumBlocks = glm::uvec3(0);
  for (size_t z = 1; z <= 512_u32 / m_pageTableBlockSize.z; ++z) {
    for (size_t y = z; y <= 512_u32 / m_pageTableBlockSize.y; ++y) {
      for (size_t x = y; x <= 512_u32 / m_pageTableBlockSize.x; ++x) {
        if (x * y * z * m_pageTableBlockSize.x * m_pageTableBlockSize.y * m_pageTableBlockSize.z * 4 * 4 >
            m_maxMemoryForPageTableCache) {
          continue;
        }
        auto currentNumBlocks =
          size_t(m_pageTableCacheNumBlocks.x) * m_pageTableCacheNumBlocks.y * m_pageTableCacheNumBlocks.z;
        auto candidateNumBlocks = x * y * z;
        if (currentNumBlocks < numPageTableBlocks) {
          if (candidateNumBlocks > currentNumBlocks) {
            m_pageTableCacheNumBlocks = glm::uvec3(x, y, z);
          }
        } else if (currentNumBlocks > numPageTableBlocks) {
          if (candidateNumBlocks >= numPageTableBlocks && candidateNumBlocks < currentNumBlocks) {
            m_pageTableCacheNumBlocks = glm::uvec3(x, y, z);
          }
        }
      }
    }
  }
  m_hasSufficientPageTableCacheSpace =
    m_pageTableCacheNumBlocks.x * m_pageTableCacheNumBlocks.y * m_pageTableCacheNumBlocks.z >= numPageTableBlocks;

  const auto imageBlockTotalSize = m_imageBlockSize + m_imageBlockSizePad;
  m_imageCacheNumBlocks = glm::uvec3(0);
  for (size_t z = 1; z <= 2048_u32 / imageBlockTotalSize.z; ++z) {
    for (size_t y = z; y <= 2048_u32 / imageBlockTotalSize.y; ++y) {
      for (size_t x = y; x <= 2048_u32 / imageBlockTotalSize.x; ++x) {
        if (x * y * z * imageBlockTotalSize.x * imageBlockTotalSize.y * imageBlockTotalSize.z >
            m_maxMemoryForImageCache) {
          continue;
        }
        auto currentNumBlocks = size_t(m_imageCacheNumBlocks.x) * m_imageCacheNumBlocks.y * m_imageCacheNumBlocks.z;
        auto candidateNumBlocks = x * y * z;
        if (currentNumBlocks < numImageBlocks) {
          if (candidateNumBlocks > currentNumBlocks) {
            m_imageCacheNumBlocks = glm::uvec3(x, y, z);
          }
        } else if (currentNumBlocks > numImageBlocks) {
          if (candidateNumBlocks >= numImageBlocks && candidateNumBlocks < currentNumBlocks) {
            m_imageCacheNumBlocks = glm::uvec3(x, y, z);
          }
        }
      }
    }
  }
  m_pageTableCacheSize = m_pageTableCacheNumBlocks * m_pageTableBlockSize;
  const glm::uvec3 imageCacheDimensions = m_imageCacheNumBlocks * imageBlockTotalSize;
  LOG(INFO) << "maxMemoryForImageCache: " << m_maxMemoryForImageCache
            << " maxMemoryForPageTableCache: " << m_maxMemoryForPageTableCache;
  LOG(INFO) << "imageCacheSize: " << imageCacheDimensions << " imageCacheNumBlocks: " << m_imageCacheNumBlocks
            << " pageTableCacheSize: " << m_pageTableCacheSize
            << " pageTableCacheNumBlocks: " << m_pageTableCacheNumBlocks;

  // CPU paging state is backend-neutral and always required for paging decisions.
  // GPU-resident paging caches (OpenGL textures / Vulkan textures) are backend-specific.
  if (m_channelPageDirectories.size() != m_nChannels) {
    m_channelPageDirectories.resize(m_nChannels);
  }
  if (m_channelPageTableCacheManagers.size() != m_nChannels) {
    m_channelPageTableCacheManagers.resize(m_nChannels);
  }
  if (m_channelPageTableCaches.size() != m_nChannels) {
    m_channelPageTableCaches.resize(m_nChannels);
  }
  if (m_channelImageCacheManagers.size() != m_nChannels) {
    m_channelImageCacheManagers.resize(m_nChannels);
  }

  // Keep GL-side vectors sized, but do not create/destroy GL resources here.
  // setScale() can be called while Vulkan is the active backend.
  if (m_channelPageDirectoryTextures.size() < m_nChannels) {
    m_channelPageDirectoryTextures.resize(m_nChannels);
  }
  if (m_channelPageTableCacheTextures.size() < m_nChannels) {
    m_channelPageTableCacheTextures.resize(m_nChannels);
  }
  if (m_channelImageCacheTextures.size() < m_nChannels) {
    m_channelImageCacheTextures.resize(m_nChannels);
  }
  if (m_glPagingTexturesDirty.size() < m_nChannels) {
    m_glPagingTexturesDirty.resize(m_nChannels, true);
  }

  for (size_t c = 0; c < m_nChannels; ++c) {
    // page directory (CPU)
    m_channelPageDirectories[c].resize(size_t(m_pageDirectorySize.x) * m_pageDirectorySize.y * m_pageDirectorySize.z);
    std::ranges::fill(m_channelPageDirectories[c], glm::uvec4(0));

    // page table (CPU)
    m_channelPageTableCacheManagers[c] =
      std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, m_invalidKey);
    m_channelPageTableCaches[c].resize(size_t(m_pageTableCacheSize.x) * m_pageTableCacheSize.y *
                                       m_pageTableCacheSize.z);
    std::ranges::fill(m_channelPageTableCaches[c], glm::uvec4(0));

    // image cache (CPU bookkeeping)
    m_channelImageCacheManagers[c] =
      std::make_unique<Z3DBlockCache<glm::uvec4>>(imageBlockTotalSize, m_imageCacheNumBlocks, m_invalidKey);

    // Any existing GL paging textures are now out-of-date with the CPU paging state.
    // Mark them dirty so OpenGL-only call sites can recreate/rehydrate them lazily
    // under a valid GL context.
    m_glPagingTexturesDirty[c] = true;
  }

  // If Vulkan paging uploader is active, its per-image resources depend on the
  // computed cache dimensions; update them proactively.
  if (m_vulkanImageBlockUploader) {
    m_vulkanImageBlockUploader->ensureImageResources(*this);
  }

  for (size_t l = 0; l < m_numLevels; ++l) {
    LOG(INFO) << l << " pageDirectoryDimension: " << m_pageDirectoryDimensions[l]
              << " m_pageDirectoryBases: " << m_pageDirectoryBases[l]
              << " pageTableDimension: " << m_pageTableDimensions[l] << " imageDimension: " << m_imageDimensions[l]
              << " levelScale: " << m_levelScales[l] << " posToBlockID: " << m_posToBlockIDs[l]
              << " voxelWorldDimension: " << m_voxelWorldDimensions[l] << " voxelWorldSize: " << m_voxelWorldSizes[l];
  }
}

void Z3DImg::setChannelDisplayRanges(const std::vector<glm::dvec2>& displayRanges)
{
  m_channelDisplayRanges = displayRanges;
  for (const auto& dr : m_channelDisplayRanges) {
    VLOG(1) << dr;
  }
  CHECK(m_imgPack.imgInfo().numChannels == m_channelDisplayRanges.size())
    << m_imgPack.imgInfo().numChannels << " " << m_channelDisplayRanges.size();
  readVolumes();

  if (m_isVolumeDownsampled) {
    for (size_t c = 0; c < m_nChannels; ++c) {
      resetCacheSystem(c);
    }
  }
}

void Z3DImg::bindFullResBlockIDsShader(Z3DShaderProgram& shader, size_t c) const
{
  const_cast<Z3DImg*>(this)->ensureGLPagingTexturesForChannel(c);
  CHECK(m_channelPageDirectoryTextures[c] != nullptr);
  CHECK(m_channelPageTableCacheTextures[c] != nullptr);

  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageDimensions.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniformArray("pos_to_block_ids", m_posToBlockIDs.data(), m_numLevels);

  shader.bindTexture("page_directory", m_channelPageDirectoryTextures[c].get());
  shader.bindTexture("page_table_cache", m_channelPageTableCacheTextures[c].get());
}

void Z3DImg::bindFullResRenderShader(Z3DShaderProgram& shader, size_t c) const
{
  const_cast<Z3DImg*>(this)->ensureGLPagingTexturesForChannel(c);
  CHECK(m_channelPageDirectoryTextures[c] != nullptr);
  CHECK(m_channelPageTableCacheTextures[c] != nullptr);
  CHECK(m_channelImageCacheTextures[c] != nullptr);

  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageDimensions.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniform("image_address_to_normalized_texture_coord",
                    1.f / glm::vec3(m_channelImageCacheTextures[c]->dimension()));

  shader.bindTexture("page_directory", m_channelPageDirectoryTextures[c].get());
  shader.bindTexture("page_table_cache", m_channelPageTableCacheTextures[c].get());
  shader.bindTexture("image_cache", m_channelImageCacheTextures[c].get());

  Z3DTexture* texture = channelTexture(c);
  CHECK(texture != nullptr) << "Missing channel texture for channel " << c;
  shader.bindTexture("volume", texture);
}

bool Z3DImg::updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                                size_t c,
                                                const folly::CancellationToken& cancellationToken,
                                                ZBenchTimer& bt,
                                                uint32_t roundIndex)
{
  const uint32_t prevRound = m_activeReadStatsRound;
  m_activeReadStatsRound = roundIndex;
  auto roundGuard = folly::makeGuard([this, prevRound]() { m_activeReadStatsRound = prevRound; });

  const auto statsContext = ZImgReadStatsContext{static_cast<uint32_t>(c), roundIndex};

  if (missingBlockIDs.empty()) {
    if (m_readStatsSink) {
      ZImgPagingRoundSummary summary;
      summary.missingBlocks = 0;
      summary.processedMissingBlocks = 0;
      summary.skippedMissingBlocks = 0;
      summary.alreadyMappedBlocks = 0;
      summary.emptyBlocksMarked = 0;
      summary.blocksQueuedForRead = 0;
      summary.emptyBlocksRead = 0;
      summary.filledAllMissingBlocks = true;
      m_readStatsSink->on3dPagingRoundSummary(statsContext, summary);
    }
    LOG(INFO) << "no missing blocks";
    return true;
  }

  if (!m_vulkanImageBlockUploader) {
    ensureGLPagingTexturesForChannel(c);
  }

  if (VLOG_IS_ON(1) && c < m_lastPagingLodStatsLogTimes.size()) {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(FLAGS_atlas_3d_paging_lod_stats_log_interval_ms);
    if (now - m_lastPagingLodStatsLogTimes[c] >= interval) {
      m_lastPagingLodStatsLogTimes[c] = now;

      std::vector<size_t> missingPerLevel;
      missingPerLevel.resize(m_numLevels, 0);
      size_t totalMissingPaged = 0;
      for (auto blockID : missingBlockIDs) {
        size_t level = 0;
        while (level + 1 < m_numLevels && blockID >= m_posToBlockIDs[level + 1].x) {
          ++level;
        }
        if (level + 1 < m_numLevels) {
          ++missingPerLevel[level];
          ++totalMissingPaged;
        }
      }

      std::string msg = fmt::format("Z3DImg paging LOD demand: channel {} total {} missing blocks",
                                    c,
                                    totalMissingPaged);
      if (m_imgPack.isNeuroglancerPrecomputed()) {
        auto vol = m_imgPack.neuroglancerVolumeShared();
        std::vector<std::array<size_t, 3>> candidates = vol->availableRatios();
        candidates.push_back({1, 1, 1});

        for (size_t l = 0; l + 1 < m_numLevels; ++l) {
          const size_t count = missingPerLevel[l];
          if (count == 0) {
            continue;
          }
          const double pct = totalMissingPaged > 0 ? (100.0 * static_cast<double>(count) / totalMissingPaged) : 0.0;

          const std::array<size_t, 3> requested = {m_levelScales[l].x, m_levelScales[l].y, m_levelScales[l].z};
          const std::array<size_t, 3> readRatio = bestRatioAtMost(candidates, requested);
          QString scaleKey = QStringLiteral("<unknown>");
          if (auto idxOpt = vol->scaleIndexForRatio(readRatio)) {
            scaleKey = vol->scales().at(*idxOpt).key;
          }

          msg += fmt::format(" | L{}: {} ({:.0f}%) req [{},{},{}] read [{},{},{}] ('{}')",
                             l,
                             count,
                             pct,
                             requested[0],
                             requested[1],
                             requested[2],
                             readRatio[0],
                             readRatio[1],
                             readRatio[2],
                             scaleKey.toStdString());
        }
      } else {
        for (size_t l = 0; l + 1 < m_numLevels; ++l) {
          const size_t count = missingPerLevel[l];
          if (count == 0) {
            continue;
          }
          const double pct = totalMissingPaged > 0 ? (100.0 * static_cast<double>(count) / totalMissingPaged) : 0.0;
          msg += fmt::format(" | L{}: {} ({:.0f}%) ratio [{},{},{}]",
                             l,
                             count,
                             pct,
                             m_levelScales[l].x,
                             m_levelScales[l].y,
                             m_levelScales[l].z);
        }
      }

      VLOG(1) << msg;
    }
  }

  auto numBlocksToRead = m_channelImageCacheManagers[c]->size();
  LOG(INFO) << "total " << m_channelImageCacheManagers[c]->size() << " need " << missingBlockIDs.size();

  checkPageSystemError(c, true);

  maybeCancel(cancellationToken);

  size_t count = 0;
  size_t alreadyMapped = 0;
  size_t alreadyEmpty = 0;
  size_t emptyBlockCount = 0;
  auto imageBlockSize = m_imageBlockSize + m_imageBlockSizePad;

  // pageDirectoryEntryKey, pageDirectoryEntry*, pageTableEntryKey
  std::vector<std::tuple<glm::uvec4, glm::uvec4*, glm::uvec4>> pendingBlocks;
  std::vector<std::tuple<glm::uvec4, glm::uvec4*>> pendingTasks; // pageTableEntryKey, pageTableEntry*
  // used to make sure used page table block will not be swapped out
  boost::unordered_flat_set<glm::uvec4> usedPageDirectoryEntryKeys;

#ifdef ATLAS_CHECK_CACHE
  boost::unordered_flat_set<glm::uvec3> usedPageDirectoryEntry;
  m_usedPageTableEntry.clear();
#endif

  for (auto blockID : missingBlockIDs) {
    if (count >= numBlocksToRead + emptyBlockCount) {
      LOG(INFO) << "no space for new image block, skip the remaining image blocks";
      break;
    }

    size_t level = 0;
    while (level + 1 < m_numLevels && blockID >= m_posToBlockIDs[level + 1].x) {
      ++level;
    }
    CHECK(level + 1 < m_numLevels);

    glm::uvec4 pageTableEntryKey(blockID, 0, 0, level);
    pageTableEntryKey.x -= m_posToBlockIDs[level].x;
    pageTableEntryKey.z = pageTableEntryKey.x / m_posToBlockIDs[level].z;
    pageTableEntryKey.x -= pageTableEntryKey.z * m_posToBlockIDs[level].z;
    pageTableEntryKey.y = pageTableEntryKey.x / m_posToBlockIDs[level].y;
    pageTableEntryKey.x -= pageTableEntryKey.y * m_posToBlockIDs[level].y;
    CHECK(glm::all(glm::lessThan(pageTableEntryKey.xyz(), m_pageTableDimensions[level])))
      << blockID << " " << pageTableEntryKey << " " << m_pageTableDimensions[level];
    auto pageDirectoryEntryKey = pageTableEntryKey / glm::uvec4(m_pageTableBlockSize, 1);
    auto pageDirectoryEntryCoord = m_pageDirectoryBases[pageDirectoryEntryKey.w] + pageDirectoryEntryKey.xyz();
#ifdef ATLAS_CHECK_CACHE
    CHECK(glm::all(glm::lessThan(pageDirectoryEntryCoord, m_pageDirectorySize)));
#endif
    auto pageDirectoryEntryPtr =
      &m_channelPageDirectories[c][pageDirectoryEntryCoord.z * m_pageDirectorySize.y * m_pageDirectorySize.x +
                                   pageDirectoryEntryCoord.y * m_pageDirectorySize.x + pageDirectoryEntryCoord.x];

    if (pageDirectoryEntryPtr->w == m_emptyFlag) { // page table is empty block
      CHECK(false); // empty page table is not supported yet
      ++emptyBlockCount;
      ++count;
      continue;
    } else if (pageDirectoryEntryPtr->w > 0) { // page table mapped
      usedPageDirectoryEntryKeys.insert(pageDirectoryEntryKey);
#ifdef ATLAS_CHECK_CACHE
      usedPageDirectoryEntry.insert(pageDirectoryEntryPtr->xyz());
#endif

      auto pageTableEntryCoord = pageDirectoryEntryPtr->xyz() + pageTableEntryKey.xyz() % m_pageTableBlockSize;
#ifdef ATLAS_CHECK_CACHE
      CHECK(glm::all(glm::lessThan(pageTableEntryCoord, m_pageTableCacheSize)));
#endif
      auto pageTableEntryPtr =
        &m_channelPageTableCaches[c][pageTableEntryCoord.z * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                     pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x];

      if (pageTableEntryPtr->w != 0) { // image block already mapped or is empty block
        if (pageTableEntryPtr->w == m_emptyFlag) {
          if (m_vulkanImageBlockUploader != nullptr) {
            // Deferred Vulkan Block-ID readbacks are generated from a paging snapshot
            // that can lag behind the CPU cache state. If another deferred readback
            // already classified this block as empty, treat this ID as stale input
            // instead of resetting the entire cache system.
            ++alreadyEmpty;
          } else {
            LOG(ERROR) << "Error: block id shader should not collect mapped empty block, "
                          "will reset the cache system and try again.";
            LOG(ERROR) << *pageDirectoryEntryPtr << " " << *pageTableEntryPtr << " " << pageTableEntryKey << " "
                       << emptyBlockCount << " " << pageDirectoryEntryKey << " " << pageDirectoryEntryCoord << " "
                       << pageTableEntryCoord; // block id shader should not collect mapped empty block
            resetCacheSystem(c);
            return updateAndUploadPageDirectoryCaches(missingBlockIDs, c, cancellationToken, bt, roundIndex);
          }
        } else {
          m_channelImageCacheManagers[c]->touch(pageTableEntryKey);
          ++alreadyMapped;
#ifdef ATLAS_CHECK_CACHE
          m_usedPageTableEntry.insert(pageTableEntryPtr->xyz());
#endif
        }
      } else { // page table mapped but image block is not mapped
        // increase pageDirectoryEntryPtr now, upload image blocks and update page table block later in pendingTasks
        ++pageDirectoryEntryPtr->w;
        if (isImageBlockEmpty(c, pageTableEntryKey, imageBlockSize)) {
          *pageTableEntryPtr = m_emptyPageTableEntry;
          ++emptyBlockCount;
        } else {
          // pageTableEntryPtr->w must be 0 here
          pendingTasks.push_back(std::make_tuple(pageTableEntryKey, pageTableEntryPtr));
        }
      }
      ++count;
    } else { // pageDirectoryEntryPtr.w == 0, page table not mapped
      pendingBlocks.push_back(std::make_tuple(pageDirectoryEntryKey, pageDirectoryEntryPtr, pageTableEntryKey));
    }
  }

  if (!m_hasSufficientPageTableCacheSpace) {
    for (const auto& pageDirectoryEntryKey : usedPageDirectoryEntryKeys) {
      // VLOG(1) << pageDirectoryEntryKey;
      m_channelPageTableCacheManagers[c]->touch(pageDirectoryEntryKey);
    }
  }
  auto numAvailablePageCacheBlock =
    std::ssize(*m_channelPageTableCacheManagers[c]) - std::ssize(usedPageDirectoryEntryKeys);
  clearAndDeallocate(usedPageDirectoryEntryKeys);

  VLOG(2) << "numAvailablePageCacheBlock: " << numAvailablePageCacheBlock;

  for (const auto& [pageDirectoryEntryKey, pageDirectoryEntryPtr, pageTableEntryKey] : pendingBlocks) {
    if (count >= numBlocksToRead + emptyBlockCount) {
      LOG(INFO) << "no space for new image block, skip the remaining image blocks";
      break;
    }

    if (pageDirectoryEntryPtr->w > 0) {
      // page table mapped in this for loop by earlier blocks, but image block is not mapped, will add a pendingTask
      ++pageDirectoryEntryPtr->w;
    } else {
      // pageDirectoryEntryPtr->w == 0, page table not mapped
      if (numAvailablePageCacheBlock > 0) {
        // we still have space, construct new page table block
        // VLOG(2) << pageDirectoryEntryKey;
        insertPageTableBlockToCache(c, pageDirectoryEntryKey, *pageDirectoryEntryPtr);
#ifdef ATLAS_CHECK_CACHE
        CHECK(!usedPageDirectoryEntry.contains(pageDirectoryEntryPtr->xyz())) << *pageDirectoryEntryPtr;
        usedPageDirectoryEntry.insert(pageDirectoryEntryPtr->xyz());
#endif
        // after insertion, pageDirectoryEntryPtr->w == 1 (not count as mapped page tables), will add a pendingTask
        ++pageDirectoryEntryPtr->w;
        --numAvailablePageCacheBlock;
      } else {
        LOG(INFO) << "no space for new page table block, skip the current image block";
        continue; // later image block in pendingBlocks might already have mapped page table so continue to process
      }
    }

    auto pageTableEntryCoord = pageDirectoryEntryPtr->xyz() + pageTableEntryKey.xyz() % m_pageTableBlockSize;
    auto pageTableEntryPtr =
      &m_channelPageTableCaches[c][pageTableEntryCoord.z * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                   pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x];
    if (isImageBlockEmpty(c, pageTableEntryKey, imageBlockSize)) {
      *pageTableEntryPtr = m_emptyPageTableEntry;
      ++emptyBlockCount;
    } else {
      // pageTableEntryPtr->w must be 0 here
      CHECK(pageTableEntryPtr->w == 0) << *pageTableEntryPtr;
      pendingTasks.push_back(std::make_tuple(pageTableEntryKey, pageTableEntryPtr));
    }
    ++count;
  }
  VLOG(2) << "pendingTasks size: " << pendingTasks.size();
  clearAndDeallocate(pendingBlocks);
  bt.recordEvent("update cache system");

  //  for (size_t i = 0; i < pendingTasks.size(); ++i) {
  //    const auto& pageTableEntryKey = std::get<0>(pendingTasks[i]);
  //    glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
  //    auto imageBlockSize = m_imageBlockSize;
  //    VLOG(1) << m_levelScales[blockImagePos.x].x * blockImagePos.y << " "
  //              << m_levelScales[blockImagePos.x].x * blockImagePos.z << " "
  //              << m_levelScales[blockImagePos.x].z * blockImagePos.w << " "
  //              << m_levelScales[blockImagePos.x].x * imageBlockSize.x << " "
  //              << m_levelScales[blockImagePos.x].x * imageBlockSize.y << " "
  //              << m_levelScales[blockImagePos.x].z * imageBlockSize.z;
  //  }

  size_t readEmptyBlockCount = 0;
  if (!pendingTasks.empty() || emptyBlockCount > 0) { // we have changed the cache system
    auto uploadGuard = folly::makeGuard([=, this, &bt]() {
      checkPageSystemError(c, false);
      if (m_readStatsSink) {
        const uint64_t pageDirectoryBytes = static_cast<uint64_t>(pageDirectoryView(c).size_bytes());
        const uint64_t pageTableBytes = static_cast<uint64_t>(pageTableCacheView(c).size_bytes());
        m_readStatsSink->onGpuUploadBytes(statsContext,
                                          ZImgGpuUploadKind::PageDirectory,
                                          pageDirectoryBytes,
                                          /*blocks=*/0);
        m_readStatsSink->onGpuUploadBytes(statsContext,
                                          ZImgGpuUploadKind::PageTableCache,
                                          pageTableBytes,
                                          /*blocks=*/0);
      }
      if (m_vulkanImageBlockUploader) {
        m_vulkanImageBlockUploader->uploadPageCaches(*this, c, bt);
      } else {
        m_channelPageDirectoryTextures[c]->updateImage(m_channelPageDirectories[c].data());
        m_channelPageTableCacheTextures[c]->updateImage(m_channelPageTableCaches[c].data());
        bt.recordEvent("upload page table");
      }
    });

    // read image
    if (!pendingTasks.empty()) {
      readEmptyBlockCount = readAndUploadImageBlocks(c, pendingTasks, cancellationToken, bt);
    }
  }

  LOG(INFO) << fmt::format(
    "filled {} blocks ({} already mapped, {} already empty, {} newly empty, read {} blocks ({} empty))",
    count,
    alreadyMapped,
    alreadyEmpty,
    emptyBlockCount,
    pendingTasks.size(),
    readEmptyBlockCount);

#ifdef ATLAS_CHECK_CACHE
  CHECK(m_usedPageTableEntry.size() == alreadyMapped + pendingTasks.size() - readEmptyBlockCount)
    << m_usedPageTableEntry.size();
#endif
  // glFinish();

  // checkPageSystemError();

  const bool filledAllMissingBlocks = (count == missingBlockIDs.size());
  if (m_readStatsSink) {
    ZImgPagingRoundSummary summary;
    summary.missingBlocks = missingBlockIDs.size();
    summary.processedMissingBlocks = count;
    summary.skippedMissingBlocks = (count < missingBlockIDs.size()) ? (missingBlockIDs.size() - count) : 0;
    summary.alreadyMappedBlocks = alreadyMapped;
    summary.emptyBlocksMarked = emptyBlockCount;
    summary.blocksQueuedForRead = pendingTasks.size();
    summary.emptyBlocksRead = readEmptyBlockCount;
    summary.filledAllMissingBlocks = filledAllMissingBlocks;
    m_readStatsSink->on3dPagingRoundSummary(statsContext, summary);
  }

  return filledAllMissingBlocks;
}

// #define ATLAS_uploadImageCache_USE_MPMCQueue

void Z3DImg::readVolumes()
{
  if (m_channelResources.size() != m_nChannels) {
    m_channelResources.resize(m_nChannels);
  }

  if (m_volumeGenerations.size() != m_nChannels) {
    m_volumeGenerations.assign(m_nChannels, 0);
  }

  const folly::CancellationToken cancellationToken = Z3DRenderGlobalState::instance().currentCancellationToken();
  std::shared_ptr<const ZImg> img;
  m_pendingPreviewWarning.reset();
  try {
    auto cpuExecutor = folly::getGlobalCPUExecutor();
    auto task = folly::coro::co_withCancellation(
      cancellationToken,
      m_imgPack.resizedImgCachedAsync(m_volumeDimension.x, m_volumeDimension.y, m_volumeDimension.z, 0));
    ZImgPack::PreviewBuildResult previewResult =
      folly::coro::blockingWait(folly::coro::co_withExecutor(cpuExecutor, std::move(task)));
    img = std::move(previewResult.image);
    m_pendingPreviewWarning = std::move(previewResult.warning);
  }
  catch (const folly::OperationCancelled&) {
    // co_withCancellation reports cancellation via folly::OperationCancelled.
    // Translate to Atlas' cancellation type so the renderer can unwind.
    throw ZCancellationException();
  }
  CHECK(img);

  if (m_nChannels == 1) {
    if (img->isType<uint8_t>() &&
        (img->validBitCount() == 0 || img->validBitCount() == 8 || img->validBitCount() == 16)) {
      auto& channel = m_channelResources[0];
      channel.image = img;
      channel.dimensions = glm::uvec3(static_cast<uint32_t>(channel.image->width()),
                                      static_cast<uint32_t>(channel.image->height()),
                                      static_cast<uint32_t>(channel.image->depth()));
      ++m_volumeGenerations[0];
      return;
    }

    ZImg mutableImg = *img;
    if (!mutableImg.isType<uint8_t>()) {
      mutableImg = mutableImg.convertTo<uint8_t>(m_channelDisplayRanges[0].x, m_channelDisplayRanges[0].y);
    } else if (mutableImg.validBitCount() != 0 && mutableImg.validBitCount() != 8 && mutableImg.validBitCount() != 16) {
      mutableImg.normalize(m_channelDisplayRanges[0].x, m_channelDisplayRanges[0].y);
    }

    auto mutableImage = std::make_shared<ZImg>(std::move(mutableImg));
    auto& channel = m_channelResources[0];
    channel.image = std::shared_ptr<const ZImg>(std::move(mutableImage));
    channel.dimensions = glm::uvec3(static_cast<uint32_t>(channel.image->width()),
                                    static_cast<uint32_t>(channel.image->height()),
                                    static_cast<uint32_t>(channel.image->depth()));
    ++m_volumeGenerations[0];
  } else {
    for (size_t i = 0; i < m_nChannels; ++i) {
      const ZImg channelView = img->createView(static_cast<index_t>(i), 0);
      ZImg cImg;
      if (!channelView.isType<uint8_t>()) {
        cImg = channelView.convertTo<uint8_t>(m_channelDisplayRanges[i].x, m_channelDisplayRanges[i].y);
      } else {
        cImg = channelView;
        if (cImg.validBitCount() != 0 && cImg.validBitCount() != 8 && cImg.validBitCount() != 16) {
          cImg.normalize(m_channelDisplayRanges[i].x, m_channelDisplayRanges[i].y);
        }
      }

      auto mutableImage = std::make_shared<ZImg>(std::move(cImg));
      auto& channel = m_channelResources[i];
      channel.image = std::shared_ptr<const ZImg>(std::move(mutableImage));
      channel.dimensions = glm::uvec3(static_cast<uint32_t>(channel.image->width()),
                                      static_cast<uint32_t>(channel.image->height()),
                                      static_cast<uint32_t>(channel.image->depth()));
      ++m_volumeGenerations[i];
    }
  }
}

void Z3DImg::insertPageTableBlockToCache(size_t c,
                                         const glm::uvec4& pageDirectoryEntryKey,
                                         glm::uvec4& pageDirectoryEntryRef)
{
#ifdef ATLAS_CHECK_CACHE
  CHECK(!m_channelPageTableCacheManagers[c]->exists(pageDirectoryEntryKey))
    << pageDirectoryEntryKey << " " << m_channelPageTableCacheManagers[c]->get(pageDirectoryEntryKey) << " "
    << pageDirectoryEntryRef;
#endif
  glm::uvec4 erasedKey;
  glm::uvec3 pageTableBlockCachePos = m_channelPageTableCacheManagers[c]->insert(pageDirectoryEntryKey, erasedKey);
  // when page table mapped but no image blocks is mapped yet, pageDirectoryEntry.w == 1
  pageDirectoryEntryRef = glm::uvec4(pageTableBlockCachePos, 1);

  if (erasedKey.x != std::numeric_limits<uint32_t>::max()) {
    CHECK(!m_hasSufficientPageTableCacheSpace) << "page table block swapped out" << pageDirectoryEntryKey << erasedKey;
    for (size_t z = 0; z < m_pageTableBlockSize.z; ++z) {
      for (size_t y = 0; y < m_pageTableBlockSize.y; ++y) {
        std::fill_n(
          &m_channelPageTableCaches[c]
                                   [(pageTableBlockCachePos.z + z) * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                    (pageTableBlockCachePos.y + y) * m_pageTableCacheSize.x + pageTableBlockCachePos.x],
          m_pageTableBlockSize.x,
          glm::uvec4(0));
      }
    }

    glm::uvec3 erasedKeyPageDirectoryEntryCoord = m_pageDirectoryBases[erasedKey.w] + erasedKey.xyz();
    glm::uvec4& erasedKeyPageDirectoryEntry =
      m_channelPageDirectories[c][erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.x];
    erasedKeyPageDirectoryEntry.w = 0;
  }
}

void Z3DImg::releaseGLResources()
{
  // Release GL-only helper objects as well (e.g. PBO used for paging uploads).
  // These are tied to the active GL context and must not survive backend switches
  // that destroy/recreate the context.
  m_PBO.reset();

  // When switching away from OpenGL, the resident full-resolution paging cache
  // contents (image blocks) live only in GL textures. The CPU page directory /
  // page table arrays encode *where* blocks are mapped, but not the underlying
  // voxel data. If we keep those mappings after releasing GL textures, a
  // different backend (Vulkan) may incorrectly assume blocks are resident and
  // skip re-paging, which leads to sampling undefined image-cache contents.
  //
  // Fix: clear paging state so the next backend repopulates caches on demand.
  if (m_isVolumeDownsampled) {
    const size_t expectedPageDirectoryEntries =
      size_t(m_pageDirectorySize.x) * m_pageDirectorySize.y * m_pageDirectorySize.z;
    const size_t expectedPageTableEntries =
      size_t(m_pageTableCacheSize.x) * m_pageTableCacheSize.y * m_pageTableCacheSize.z;
    const glm::uvec3 imageBlockTotal = m_imageBlockSize + m_imageBlockSizePad;

    VLOG(1) << "Clearing paging CPU state for backend switch: channels=" << m_nChannels
            << ", pageDirectoryEntries=" << expectedPageDirectoryEntries << " (" << m_pageDirectorySize << ")"
            << ", pageTableEntries=" << expectedPageTableEntries << " (" << m_pageTableCacheSize << ")"
            << ", imageCacheSize=" << (m_imageCacheNumBlocks * imageBlockTotal) << " (blocks " << m_imageCacheNumBlocks
            << ", blockExtent " << imageBlockTotal << ")";

    // Keep vectors sized correctly and zero-filled for all channels.
    if (m_channelPageDirectories.size() != m_nChannels) {
      m_channelPageDirectories.resize(m_nChannels);
    }
    if (m_channelPageTableCaches.size() != m_nChannels) {
      m_channelPageTableCaches.resize(m_nChannels);
    }
    if (m_channelPageTableCacheManagers.size() != m_nChannels) {
      m_channelPageTableCacheManagers.resize(m_nChannels);
    }
    if (m_channelImageCacheManagers.size() != m_nChannels) {
      m_channelImageCacheManagers.resize(m_nChannels);
    }

    for (size_t c = 0; c < m_nChannels; ++c) {
      m_channelPageDirectories[c].resize(expectedPageDirectoryEntries);
      std::ranges::fill(m_channelPageDirectories[c], glm::uvec4(0));

      m_channelPageTableCaches[c].resize(expectedPageTableEntries);
      std::ranges::fill(m_channelPageTableCaches[c], glm::uvec4(0));

      m_channelPageTableCacheManagers[c] =
        std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, m_invalidKey);
      m_channelImageCacheManagers[c] =
        std::make_unique<Z3DBlockCache<glm::uvec4>>(imageBlockTotal, m_imageCacheNumBlocks, m_invalidKey);
    }

#ifdef ATLAS_CHECK_CACHE
    m_usedPageTableEntry.clear();
#endif
  }

  // Release per-channel GL textures
  for (auto& ch : m_channelResources) {
    ch.texture.reset();
    ch.textureGeneration = 0;
  }
  // Release paging-related GL textures (if any)
  for (auto& tex : m_channelPageDirectoryTextures) {
    tex.reset();
  }
  for (auto& tex : m_channelPageTableCacheTextures) {
    tex.reset();
  }
  for (auto& tex : m_channelImageCacheTextures) {
    tex.reset();
  }
}

void Z3DImg::insertImageBlockToCache(size_t c, const glm::uvec4& pageTableEntryKey, glm::uvec4& pageTableEntryRef)
{
  // auto hasher = boost::hash<glm::uvec4>();
  // VLOG(1) << hasher(pageTableEntryKey);
#ifdef ATLAS_CHECK_CACHE
  CHECK(!m_channelImageCacheManagers[c]->exists(pageTableEntryKey))
    << pageTableEntryKey << " " << m_channelImageCacheManagers[c]->get(pageTableEntryKey) << " " << pageTableEntryRef;
#endif
  glm::uvec4 erasedKey;
  glm::uvec3 imageBlockCachePos = m_channelImageCacheManagers[c]->insert(pageTableEntryKey, erasedKey);
  // VLOG(1) << c << " " <<  pageTableEntryKey << " " << erasedKey << " " << imageBlockCachePos;
  pageTableEntryRef = glm::uvec4(imageBlockCachePos, 1);
  // VLOG(1) << blockKey << " " << erasedKey << " " << m_posToBlockIDs[level] << " " << blockID << " " <<
  // level;
  if (erasedKey.x != std::numeric_limits<uint32_t>::max()) { // valid
    glm::uvec4 erasedKeyPageDirectoryEntryKey = erasedKey / glm::uvec4(m_pageTableBlockSize, 1);
    glm::uvec3 erasedKeyPageDirectoryEntryCoord =
      m_pageDirectoryBases[erasedKeyPageDirectoryEntryKey.w] + erasedKeyPageDirectoryEntryKey.xyz();
    glm::uvec4& erasedKeyPageDirectoryEntry =
      m_channelPageDirectories[c][erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.x];

#ifdef ATLAS_CHECK_CACHE
    CHECK(erasedKeyPageDirectoryEntry.w != m_emptyFlag) << erasedKeyPageDirectoryEntry;
    if (m_hasSufficientPageTableCacheSpace) {
      CHECK(erasedKeyPageDirectoryEntry.w > 1)
        << erasedKeyPageDirectoryEntry; // should be mapped and has at least one image block so > 1
    }
#endif
    if (erasedKeyPageDirectoryEntry.w > 0) {
      glm::uvec3 erasedKeyPageTableEntryCoord =
        erasedKeyPageDirectoryEntry.xyz() + erasedKey.xyz() % m_pageTableBlockSize;
      glm::uvec4& erasedKeyPageTableEntry =
        m_channelPageTableCaches[c][erasedKeyPageTableEntryCoord.z * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                    erasedKeyPageTableEntryCoord.y * m_pageTableCacheSize.x +
                                    erasedKeyPageTableEntryCoord.x];
#ifdef ATLAS_CHECK_CACHE
      CHECK(erasedKeyPageTableEntry.w != m_emptyFlag) << erasedKeyPageTableEntry;
      if (m_hasSufficientPageTableCacheSpace) {
        CHECK(erasedKeyPageTableEntry.w > 0) << erasedKeyPageTableEntry;
      }
#endif
      if (erasedKeyPageTableEntry.w > 0) {
#ifdef ATLAS_CHECK_CACHE
        CHECK(erasedKeyPageTableEntry.w == 1) << erasedKeyPageTableEntry;
#endif
        erasedKeyPageTableEntry.w = 0;
        --erasedKeyPageDirectoryEntry.w;
        CHECK(erasedKeyPageDirectoryEntry.w >= 1);
      }
    }
  }
}

folly::coro::Task<void>
Z3DImg::readImageBlockToBufferAsync(size_t c,
                                    const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                    size_t taskIdx,
                                    const ZImgInfo& resInfo,
                                    uint8_t* buffer,
                                    std::optional<std::string>* failureMessage) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  const auto& pageTableEntryKey = std::get<0>(pendingTasks[taskIdx]);
  auto pageTableEntryPtr = std::get<1>(pendingTasks[taskIdx]);
  glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
  const auto statsContext = ZImgReadStatsContext{static_cast<uint32_t>(c), m_activeReadStatsRound};
  try {
    auto img = co_await m_imgPack.readRegionToImgAsync(m_levelScales[blockImagePos.w].x,
                                                       m_levelScales[blockImagePos.w].z,
                                                       index_t(blockImagePos.x) - index_t(m_imageBlockSizePad.x) / 2,
                                                       index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.y) / 2,
                                                       index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.z) / 2,
                                                       c,
                                                       0,
                                                       resInfo,
                                                       m_channelDisplayRanges[c].x,
                                                       m_channelDisplayRanges[c].y,
                                                       m_readStatsSink,
                                                       statsContext);

    maybeCancel(cancellationToken);
    if (!img) {
      *pageTableEntryPtr = m_emptyPageTableEntry;
    } else {
      std::copy_n(img->channelData(0), resInfo.byteNumber(), buffer + taskIdx * resInfo.byteNumber());
    }
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    *pageTableEntryPtr = m_emptyPageTableEntry;
    *failureMessage = e.what();
  }
}

folly::coro::Task<void>
Z3DImg::readImageBlocksToBufferAsync(size_t c,
                                     const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                     const ZImgInfo& resInfo,
                                     uint8_t* buffer,
                                     std::vector<std::optional<std::string>>& failureMessages) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  size_t effectiveBatchSize = m_blockUploadingBatchSize;
  if (m_imgPack.isNeuroglancerPrecomputed()) {
    effectiveBatchSize = std::min(effectiveBatchSize, static_cast<size_t>(FLAGS_atlas_ng_precomputed_3d_max_concurrent_block_reads));
  }
  CHECK(effectiveBatchSize > 0);

  size_t taskIdx = 0;
  while (taskIdx < pendingTasks.size()) {
    maybeCancel(cancellationToken);

    size_t numberOfTasks = std::min(effectiveBatchSize, pendingTasks.size() - taskIdx);

    std::vector<folly::coro::TaskWithExecutor<void>> blockTasks;
    blockTasks.reserve(numberOfTasks);
    size_t finalTaskIdx = taskIdx + numberOfTasks;
    for (; taskIdx < finalTaskIdx; ++taskIdx) {
      blockTasks.push_back(folly::coro::co_withExecutor(
        folly::getGlobalCPUExecutor(),
        readImageBlockToBufferAsync(c, pendingTasks, taskIdx, resInfo, buffer, &failureMessages[taskIdx])));
    }
    co_await folly::coro::collectAllRange(std::move(blockTasks));
    maybeCancel(cancellationToken);
    LOG(INFO) << fmt::format("finish block {}", taskIdx);
  }
}

template<typename QueueType>
folly::coro::Task<void>
Z3DImg::readImageBlockToQueueAsync(size_t c,
                                   const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                   size_t taskIdx,
                                   const ZImgInfo& resInfo,
                                   QueueType& queue) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  const auto& pageTableEntryKey = std::get<0>(pendingTasks[taskIdx]);
  auto pageTableEntryPtr = std::get<1>(pendingTasks[taskIdx]);
  glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
  const auto statsContext = ZImgReadStatsContext{static_cast<uint32_t>(c), m_activeReadStatsRound};
  try {
    auto img = co_await m_imgPack.readRegionToImgAsync(m_levelScales[blockImagePos.w].x,
                                                       m_levelScales[blockImagePos.w].z,
                                                       index_t(blockImagePos.x) - index_t(m_imageBlockSizePad.x) / 2,
                                                       index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.y) / 2,
                                                       index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.z) / 2,
                                                       c,
                                                       0,
                                                       resInfo,
                                                       m_channelDisplayRanges[c].x,
                                                       m_channelDisplayRanges[c].y,
                                                       m_readStatsSink,
                                                       statsContext);

    maybeCancel(cancellationToken);
    queue.enqueue(std::make_tuple(taskIdx, std::move(img), std::optional<std::string>{}));
  }
  catch (const ZCancellationException&) {
    throw;
  }
  catch (const folly::OperationCancelled&) {
    throw;
  }
  catch (const std::exception& e) {
    *pageTableEntryPtr = m_emptyPageTableEntry;
    queue.enqueue(std::make_tuple(taskIdx, std::shared_ptr<ZImg>{}, std::optional<std::string>(e.what())));
  }
}

template<typename QueueType>
folly::coro::Task<void>
Z3DImg::readImageBlocksToQueueAsync(size_t c,
                                    const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                    const ZImgInfo& resInfo,
                                    QueueType& queue,
                                    ZBenchTimer& bt) const
{
  auto cancellationToken = co_await folly::coro::co_current_cancellation_token;
  maybeCancel(cancellationToken);

  size_t effectiveBatchSize = m_blockUploadingBatchSize;
  if (m_imgPack.isNeuroglancerPrecomputed()) {
    effectiveBatchSize = std::min(effectiveBatchSize, static_cast<size_t>(FLAGS_atlas_ng_precomputed_3d_max_concurrent_block_reads));
  }
  CHECK(effectiveBatchSize > 0);

  size_t taskIdx = 0;
  while (taskIdx < pendingTasks.size()) {
    maybeCancel(cancellationToken);

    const size_t numberOfTasks = std::min(effectiveBatchSize, pendingTasks.size() - taskIdx);
    std::vector<folly::coro::TaskWithExecutor<void>> blockTasks;
    blockTasks.reserve(numberOfTasks);

    const size_t finalTaskIdx = taskIdx + numberOfTasks;
    for (; taskIdx < finalTaskIdx; ++taskIdx) {
      maybeCancel(cancellationToken);
      blockTasks.push_back(
        folly::coro::co_withExecutor(folly::getGlobalCPUExecutor(),
                                     readImageBlockToQueueAsync(c, pendingTasks, taskIdx, resInfo, queue)));
    }

    co_await folly::coro::collectAllRange(std::move(blockTasks));
  }

  maybeCancel(cancellationToken);
  LOG(INFO) << "image blocks reading finished.";
  bt.recordEvent("image blocks reading");
}

size_t Z3DImg::readAndUploadImageBlocks(size_t c,
                                        const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                        const folly::CancellationToken& cancellationToken,
                                        ZBenchTimer& bt)
{
  const auto statsContext = ZImgReadStatsContext{static_cast<uint32_t>(c), m_activeReadStatsRound};

  auto imageBlockSize = m_imageBlockSize + m_imageBlockSizePad;
  ZImgInfo resInfo(imageBlockSize.x, imageBlockSize.y, imageBlockSize.z, 1);
  const size_t blockSizeInByte = resInfo.byteNumber();

  if (m_vulkanImageBlockUploader != nullptr) {
    VLOG(3) << "using Vulkan image block uploader";
    const size_t emptyBlockCount =
      m_vulkanImageBlockUploader->readAndUploadImageBlocks(*this, c, pendingTasks, cancellationToken, bt);
    CHECK_LE(emptyBlockCount, pendingTasks.size());
    if (m_readStatsSink) {
      const uint64_t uploadedBlocks = static_cast<uint64_t>(pendingTasks.size() - emptyBlockCount);
      m_readStatsSink->onGpuUploadBytes(statsContext,
                                        ZImgGpuUploadKind::ImageBlocks,
                                        uploadedBlocks * static_cast<uint64_t>(blockSizeInByte),
                                        uploadedBlocks);
    }
    return emptyBlockCount;
  }

  ensureGLPagingTexturesForChannel(c);
  CHECK(m_channelImageCacheTextures[c] != nullptr);

  size_t emptyBlockCount = 0;

  maybeCancel(cancellationToken);

  LOG(INFO) << "reading " << pendingTasks.size() << " image blocks...";
  if (VLOG_IS_ON(1)) {
    std::vector<size_t> perLevel;
    perLevel.resize(m_numLevels, 0);
    for (const auto& task : pendingTasks) {
      const glm::uvec4& pageTableEntryKey = std::get<0>(task);
      CHECK(pageTableEntryKey.w < m_numLevels) << pageTableEntryKey.w << " " << m_numLevels;
      ++perLevel[pageTableEntryKey.w];
    }

    if (m_imgPack.isNeuroglancerPrecomputed()) {
      auto vol = m_imgPack.neuroglancerVolumeShared();
      std::vector<std::array<size_t, 3>> candidates = vol->availableRatios();
      candidates.push_back({1, 1, 1});

      std::string msg = fmt::format("Z3DImg paging (Neuroglancer): channel {} batch {} blocks", c, pendingTasks.size());
      for (size_t level = 0; level < perLevel.size(); ++level) {
        if (perLevel[level] == 0) {
          continue;
        }
        const std::array<size_t, 3> requested = {m_levelScales[level].x, m_levelScales[level].y, m_levelScales[level].z};
        const std::array<size_t, 3> readRatio = bestRatioAtMost(candidates, requested);
        QString scaleKey = QStringLiteral("<unknown>");
        if (auto idxOpt = vol->scaleIndexForRatio(readRatio)) {
          scaleKey = vol->scales().at(*idxOpt).key;
        }

        msg += fmt::format(" | L{}: {} blocks, req [{},{},{}], read [{},{},{}] (scale '{}')",
                           level,
                           perLevel[level],
                           requested[0],
                           requested[1],
                           requested[2],
                           readRatio[0],
                           readRatio[1],
                           readRatio[2],
                           scaleKey.toStdString());
      }
      VLOG(1) << msg;
    } else {
      std::string msg = fmt::format("Z3DImg paging: channel {} batch {} blocks", c, pendingTasks.size());
      for (size_t level = 0; level < perLevel.size(); ++level) {
        if (perLevel[level] == 0) {
          continue;
        }
        msg += fmt::format(" | L{}: {} blocks, ratio [{},{},{}]",
                           level,
                           perLevel[level],
                           m_levelScales[level].x,
                           m_levelScales[level].y,
                           m_levelScales[level].z);
      }
      VLOG(1) << msg;
    }
  }

  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto p = dynamic_cast<folly::ThreadPoolExecutor*>(cpuExecutor.get());
  CHECK(p);

  //  if (auto p = dynamic_cast<folly::CPUThreadPoolExecutor*>(cpuExecutor.get()); p) {
  //    VLOG(1) << "number of priorities: " << static_cast<int>(p->getNumPriorities());
  //  }

  uint8_t* pboPtr = nullptr;
  std::vector<uint8_t> pboLocalBuffer;
  if (pendingTasks.size() >= FLAGS_atlas_number_of_blocks_to_use_PBO_threashold) {
    if (!m_PBO) {
      m_PBO = std::make_unique<Z3DVertexBufferObject>();
    }
    m_PBO->bind(GL_PIXEL_UNPACK_BUFFER);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, blockSizeInByte * pendingTasks.size(), nullptr, GL_STREAM_DRAW);
    pboPtr = (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (!pboPtr) {
      LOG(WARNING) << "glMapBuffer failed on PBO";
    } else {
      pboLocalBuffer.resize(blockSizeInByte * pendingTasks.size());
    }
  }
  auto pboGuard = folly::makeGuard([=, this]() {
    if (m_PBO) {
      m_PBO->release(GL_PIXEL_UNPACK_BUFFER);
    }
  });

  if (!pboPtr) {
    maybeCancel(cancellationToken);

    folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<ZImg>, std::optional<std::string>>, true> imgQueue;
#if 1
    // auto f = folly::coro::co_withCancellation(cancellationToken,
    //                                           readImageBlocksToQueueAsync(c, pendingTasks, resInfo, imgQueue, bt))
    //            .scheduleOn(folly::getGlobalCPUExecutor())
    //            .start();
    auto readFuture = folly::coro::toFuture(
      folly::coro::co_withCancellation(cancellationToken,
                                       readImageBlocksToQueueAsync(c, pendingTasks, resInfo, imgQueue, bt)),
      folly::getGlobalCPUExecutor());
    // The reader coroutine captures references to imgQueue/pendingTasks/bt. If we return early (cancellation),
    // ensure it is drained before those locals go out of scope to avoid use-after-free memory corruption.
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
        VLOG(2) << "3D paging block reader cancelled.";
        return;
      }
      LOG(ERROR) << "3D paging block reader failed: " << result.exception().what();
    });
#else
    std::vector<folly::Future<folly::Unit>> blockFutures;
    blockFutures.reserve(pendingTasks.size());
    for (size_t i = 0; i < pendingTasks.size(); ++i) {
      const auto& pageTableEntryKey = std::get<0>(pendingTasks[i]);
      glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
      blockFutures.push_back(folly::via(cpuExecutor, [=, this, &imgQueue, &resInfo]() {
        return m_imgPack
          .readRegionToImg(m_levelScales[blockImagePos.w].x,
                           m_levelScales[blockImagePos.w].z,
                           index_t(blockImagePos.x) - index_t(m_imageBlockSizePad.x) / 2,
                           index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.y) / 2,
                           index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.z) / 2,
                           c,
                           0,
                           resInfo,
                           m_channelDisplayRanges[c].x,
                           m_channelDisplayRanges[c].y,
                           cancellationToken)
          .thenValueInline([=, &imgQueue](std::shared_ptr<ZImg>&& img) {
            maybeCancel(cancellationToken);
            imgQueue.enqueue(std::make_tuple(i, std::move(img)));
          });
      }));
    }
    auto f = folly::collect(blockFutures).via(cpuExecutor).thenValue([=, &bt](auto&&) {
      maybeCancel(cancellationToken);
      LOG(INFO) << "image blocks reading finished.";
      bt.recordEvent("image blocks reading");
    });
#endif

    maybeCancel(cancellationToken);

    std::tuple<size_t, std::shared_ptr<ZImg>, std::optional<std::string>> elem;
    auto lastLogTime = std::chrono::steady_clock::now();
    int remainingBlocksToUpload = static_cast<int>(pendingTasks.size());
    while (remainingBlocksToUpload > 0) {
      maybeCancel(cancellationToken);

      if (imgQueue.try_dequeue_until(
            elem,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(FLAGS_atlas_3d_paging_queue_poll_interval_ms))) {
        const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[std::get<0>(elem)];
        if (std::get<2>(elem).has_value()) {
          recordPagingFailure(pageTableEntryKey, *std::get<2>(elem));
        }
        if (!std::get<1>(elem)) {
          ++emptyBlockCount;
          *pageTableEntryPtr = m_emptyPageTableEntry;
        } else {
          insertImageBlockToCache(c, pageTableEntryKey, *pageTableEntryPtr);
          m_channelImageCacheTextures[c]->updateSubImage(pageTableEntryPtr->xyz(),
                                                         imageBlockSize,
                                                         std::get<1>(elem)->channelData(0));
        }
        --remainingBlocksToUpload;
      } else if (readFuture.isReady() && imgQueue.size() == 0) {
        // Reader finished but no more blocks will arrive. Break to surface the underlying error
        // (or crash if it finished "successfully" but failed to enqueue all blocks).
        break;
      }
      if (std::chrono::steady_clock::now() - lastLogTime >=
          std::chrono::seconds(FLAGS_atlas_log_folly_global_executor_status_interval_in_seconds)) {
        auto poolStats = p->getPoolStats();
        LOG(INFO) << fmt::format(
          "pending/total task count: {}/{}, active/idle thread count: {}/{}, ready/remaining blocks: {}/{}",
          poolStats.pendingTaskCount,
          poolStats.totalTaskCount,
          poolStats.activeThreadCount,
          poolStats.idleThreadCount,
          imgQueue.size(),
          remainingBlocksToUpload);
        lastLogTime = std::chrono::steady_clock::now();
      }
    }
    // Ensure the reader finished and propagate any unexpected failures.
    std::move(readFuture).get();
    CHECK(remainingBlocksToUpload == 0) << "3D paging block reader ended before delivering all blocks";
    bt.recordEvent("image blocks uploading");
  } else {
    // use PBO
    { // scope for glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      auto bufferGuard = folly::makeGuard([]() {
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      });

      maybeCancel(cancellationToken);

      CHECK(m_blockUploadingBatchSize > 0);

#if 1
      try {
        std::vector<std::optional<std::string>> failureMessages(pendingTasks.size());
        folly::coro::blockingWait(folly::coro::co_withCancellation(
          cancellationToken,
          readImageBlocksToBufferAsync(c, pendingTasks, resInfo, pboLocalBuffer.data(), failureMessages)));

        for (size_t i = 0; i < pendingTasks.size(); ++i) {
          if (failureMessages[i].has_value()) {
            recordPagingFailure(std::get<0>(pendingTasks[i]), *failureMessages[i]);
          }
        }
      }
      catch (const folly::OperationCancelled&) {
        // co_withCancellation reports cancellation via folly::OperationCancelled.
        // Translate to Atlas' cancellation type so higher-level paging/rendering
        // code can handle it consistently.
        throw ZCancellationException();
      }
#else
      size_t taskIdx = 0;
      while (taskIdx < pendingTasks.size()) {
        maybeCancel(cancellationToken);

        size_t numberOfTasks = std::min(m_blockUploadingBatchSize, pendingTasks.size() - taskIdx);
        std::vector<folly::Future<folly::Unit>> blockFutures;
        blockFutures.reserve(numberOfTasks);
        size_t finalTaskIdx = taskIdx + numberOfTasks;
        for (; taskIdx < finalTaskIdx; ++taskIdx) {
          const auto& pageTableEntryKey = std::get<0>(pendingTasks[taskIdx]);
          auto pageTableEntryPtr = std::get<1>(pendingTasks[taskIdx]);
          glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
          blockFutures.push_back(folly::via(cpuExecutor, [=, this, &resInfo, &pboLocalBuffer]() {
            return m_imgPack
              .readRegionToImg(m_levelScales[blockImagePos.w].x,
                               m_levelScales[blockImagePos.w].z,
                               index_t(blockImagePos.x) - index_t(m_imageBlockSizePad.x) / 2,
                               index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.y) / 2,
                               index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.z) / 2,
                               c,
                               0,
                               resInfo,
                               m_channelDisplayRanges[c].x,
                               m_channelDisplayRanges[c].y,
                               cancellationToken)
              .thenValueInline([=, this, &pboLocalBuffer](std::shared_ptr<ZImg>&& img) {
                maybeCancel(cancellationToken);
                if (!img) {
                  *pageTableEntryPtr = m_emptyPageTableEntry;
                } else {
                  std::copy_n(img->channelData(0), blockSizeInByte, pboLocalBuffer.data() + taskIdx * blockSizeInByte);
                }
              });
          }));
        }
        auto f = folly::collect(blockFutures).via(cpuExecutor).thenValueInline([=](auto&&) {
          maybeCancel(cancellationToken);
          LOG(INFO) << fmt::format("finish block {}", taskIdx);
        });

        while (
          !f.wait(std::chrono::seconds(FLAGS_atlas_log_folly_global_executor_status_interval_in_seconds)).isReady()) {
          maybeCancel(cancellationToken);

          auto poolStats = p->getPoolStats();
          LOG(INFO) << fmt::format("pending/total task count: {}/{}, active/idle thread count: {}/{}",
                                   poolStats.pendingTaskCount,
                                   poolStats.totalTaskCount,
                                   poolStats.activeThreadCount,
                                   poolStats.idleThreadCount);
        }
      }
#endif

      maybeCancel(cancellationToken);

      //      if (true) {
      //        auto [minv, maxv] = std::ranges::minmax_element(pboLocalBuffer);
      //        if (*maxv > 200) {
      //          CHECK(false) << *maxv << " " << *minv;
      //        }
      //      }
      std::copy_n(pboLocalBuffer.data(), pboLocalBuffer.size(), pboPtr);
      clearAndDeallocate(pboLocalBuffer);
      LOG(INFO) << "image blocks reading finished.";
      bt.recordEvent("image blocks reading to PBO");
    }

    for (size_t i = 0; i < pendingTasks.size(); ++i) {
      maybeCancel(cancellationToken);

      const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[i];
      if (pageTableEntryPtr->w == m_emptyFlag) {
        ++emptyBlockCount;
        continue;
      }
      insertImageBlockToCache(c, pageTableEntryKey, *pageTableEntryPtr);
#ifdef ATLAS_CHECK_CACHE
      CHECK(!m_usedPageTableEntry.contains(pageTableEntryPtr->xyz())) << pageTableEntryPtr->xyz();
      m_usedPageTableEntry.insert(pageTableEntryPtr->xyz());
#endif
      m_channelImageCacheTextures[c]->updateSubImage(pageTableEntryPtr->xyz(),
                                                     imageBlockSize,
                                                     (const void*)(i * blockSizeInByte));
    }
    bt.recordEvent("PBO uploading");
  }

  CHECK_LE(emptyBlockCount, pendingTasks.size());
  if (m_readStatsSink) {
    const uint64_t uploadedBlocks = static_cast<uint64_t>(pendingTasks.size() - emptyBlockCount);
    m_readStatsSink->onGpuUploadBytes(statsContext,
                                      ZImgGpuUploadKind::ImageBlocks,
                                      uploadedBlocks * static_cast<uint64_t>(blockSizeInByte),
                                      uploadedBlocks);
  }

  return emptyBlockCount;
}

void Z3DImg::setVulkanImageBlockUploader(ZVulkanImageBlockUploader* uploader)
{
  m_vulkanImageBlockUploader = uploader;
  if (m_vulkanImageBlockUploader) {
    m_vulkanImageBlockUploader->ensureImageResources(*this);
  }
}

void Z3DImg::clearPendingPagingWarnings()
{
  m_pendingPagingWarnings.clear();
}

std::optional<std::string> Z3DImg::takePendingPagingWarning()
{
  if (m_pendingPagingWarnings.empty()) {
    return std::nullopt;
  }

  std::string warning = fmt::format(
    "3D image paging exhausted retries for {} block{}. Atlas treated those blocks as empty so rendering could "
    "finish.\n\nFailures:\n{}",
    m_pendingPagingWarnings.size(),
    (m_pendingPagingWarnings.size() == 1) ? "" : "s",
    fmt::join(m_pendingPagingWarnings, "\n\n"));

  m_pendingPagingWarnings.clear();
  return warning;
}

std::optional<std::string> Z3DImg::takePendingPreviewWarning()
{
  if (!m_pendingPreviewWarning.has_value()) {
    return std::nullopt;
  }

  std::optional<std::string> warning = std::move(m_pendingPreviewWarning);
  m_pendingPreviewWarning.reset();
  return warning;
}

void Z3DImg::recordPagingFailure(const glm::uvec4& pageTableEntryKey, std::string errorMessage)
{
  LOG(ERROR) << "3D paging exhausted retries; treating failed block as empty for this render: " << pageTableEntryKey
             << " " << errorMessage;
  m_pendingPagingWarnings.push_back(fmt::format("block ({}, {}, {}, {}) failed after retries:\n{}",
                                                pageTableEntryKey.x,
                                                pageTableEntryKey.y,
                                                pageTableEntryKey.z,
                                                pageTableEntryKey.w,
                                                errorMessage));
}

void Z3DImg::checkPageSystemError(size_t c, bool strict)
{
#ifdef ATLAS_CHECK_CACHE
  boost::unordered_flat_set<glm::uvec3> usedPageDirectoryEntry;
  boost::unordered_flat_set<glm::uvec3> usedPageTableEntry;
#endif
  auto imageBlockSize = m_imageBlockSize + m_imageBlockSizePad;
  VLOG(2) << "checkPageSystemError enter: strict=" << strict << ", channel=" << c << ", pdSize=" << m_pageDirectorySize
          << ", ptSize=" << m_pageTableCacheSize << ", ibSize=" << imageBlockSize;
  for (size_t i = 0; i < m_channelPageDirectories[c].size(); ++i) {
    if (m_channelPageDirectories[c][i].w == 0) {
      continue;
    }
    CHECK(m_channelPageDirectories[c][i].w != m_emptyFlag);

    glm::uvec3 pdLoc;
    pdLoc.x = i;
    pdLoc.z = pdLoc.x / m_pageDirectorySize.x / m_pageDirectorySize.y;
    pdLoc.x -= pdLoc.z * m_pageDirectorySize.x * m_pageDirectorySize.y;
    pdLoc.y = pdLoc.x / m_pageDirectorySize.x;
    pdLoc.x -= pdLoc.y * m_pageDirectorySize.x;

    size_t level = 100000;

    for (size_t l = 0; l < m_numLevels; ++l) {
      if (glm::all(glm::greaterThanEqual(pdLoc, m_pageDirectoryBases[l])) &&
          glm::all(glm::lessThan(pdLoc, m_pageDirectoryBases[l] + m_pageDirectoryDimensions[l]))) {
        level = l;
        pdLoc -= m_pageDirectoryBases[l];
        break;
      }
    }

    CHECK(level + 1 < m_numLevels);

    glm::uvec4 pageTableKey(pdLoc, level);
    CHECK(m_channelPageTableCacheManagers[c]->exists(pageTableKey));
    CHECK(m_channelPageTableCacheManagers[c]->get(pageTableKey) == m_channelPageDirectories[c][i].xyz());
    CHECK(
      glm::all(glm::lessThanEqual(m_channelPageDirectories[c][i].xyz() + m_pageTableBlockSize, m_pageTableCacheSize)));
#ifdef ATLAS_CHECK_CACHE
    CHECK(!usedPageDirectoryEntry.contains(m_channelPageDirectories[c][i].xyz()));
    usedPageDirectoryEntry.insert(m_channelPageDirectories[c][i].xyz());
#endif

    uint32_t numValidEntry = 0;
    uint32_t numEmptyEntry = 0;
    for (size_t z = 0; z < m_pageTableBlockSize.z; ++z) {
      for (size_t y = 0; y < m_pageTableBlockSize.y; ++y) {
        for (size_t x = 0; x < m_pageTableBlockSize.x; ++x) {
          glm::uvec4 pageTableEntry =
            m_channelPageTableCaches[c][(m_channelPageDirectories[c][i].z + z) * m_pageTableCacheSize.y *
                                          m_pageTableCacheSize.x +
                                        (m_channelPageDirectories[c][i].y + y) * m_pageTableCacheSize.x +
                                        m_channelPageDirectories[c][i].x + x];
          if (pageTableEntry.w > 0) {
            ++numValidEntry;
            if (pageTableEntry.w != m_emptyFlag) {
              glm::uvec4 imageCacheKey(glm::uvec3(x, y, z) + pdLoc * m_pageTableBlockSize, level);
              CHECK(m_channelImageCacheManagers[c]->exists(imageCacheKey)) << imageCacheKey;
              CHECK(m_channelImageCacheManagers[c]->get(imageCacheKey) == pageTableEntry.xyz());
              // Use CPU-known cache dimensions instead of GL texture dims to avoid
              // dereferencing GL textures in Vulkan mode.
              {
                const glm::uvec3 cacheDims = m_imageCacheNumBlocks * (m_imageBlockSize + m_imageBlockSizePad);
                CHECK(glm::all(glm::lessThanEqual(pageTableEntry.xyz() + imageBlockSize, cacheDims)));
              }
#ifdef ATLAS_CHECK_CACHE
              CHECK(!usedPageTableEntry.contains(pageTableEntry.xyz()));
              usedPageTableEntry.insert(pageTableEntry.xyz());
#endif
            } else {
              ++numEmptyEntry;
            }
          }
        }
      }
    }
    if (strict) {
      CHECK(numValidEntry + 1 == m_channelPageDirectories[c][i].w)
        << numValidEntry << " " << m_channelPageDirectories[c][i].w << " " << i << " " << numEmptyEntry;
    } else {
      CHECK(numValidEntry + 1 <= m_channelPageDirectories[c][i].w)
        << numValidEntry << " " << m_channelPageDirectories[c][i].w << " " << i << " " << numEmptyEntry;
      if (numValidEntry + 1 != m_channelPageDirectories[c][i].w) {
        LOG(INFO) << "Fixing cancellation caused inconsistency";
        m_channelPageDirectories[c][i].w = numValidEntry + 1;
      }
    }
  }
  VLOG(2) << "checkPageSystemError finished: channel=" << c;
}

void Z3DImg::resetCacheSystem(size_t c)
{
  m_channelPageTableCacheManagers[c] =
    std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, m_invalidKey);
  m_channelImageCacheManagers[c] = std::make_unique<Z3DBlockCache<glm::uvec4>>(m_imageBlockSize + m_imageBlockSizePad,
                                                                               m_imageCacheNumBlocks,
                                                                               m_invalidKey);
  std::ranges::fill(m_channelPageDirectories[c], glm::uvec4(0));
  std::ranges::fill(m_channelPageTableCaches[c], glm::uvec4(0));
  // IMPORTANT:
  // This function is called from backend-agnostic code paths (e.g. changing
  // channel display ranges) and therefore must not unconditionally touch OpenGL.
  // When Atlas starts in Vulkan mode, there may be no active GL context, and
  // attempting to create/update GL textures here will crash (Apple GL driver can
  // throw std::out_of_range from deep within GL entrypoints).
  //
  // Instead, mark the GL paging textures dirty and let OpenGL-only call sites
  // (bind shaders / updateAndUploadPageDirectoryCaches) recreate+upload lazily
  // under a valid GL context.
  if (m_glPagingTexturesDirty.size() < m_nChannels) {
    m_glPagingTexturesDirty.resize(m_nChannels, 1u);
  }
  m_glPagingTexturesDirty[c] = 1u;
}

void Z3DImg::ensureGLPagingTexturesForChannel(size_t c)
{
  CHECK(m_isVolumeDownsampled);
  CHECK_LT(c, m_nChannels);
  CHECK(m_vulkanImageBlockUploader == nullptr) << "GL paging textures requested while Vulkan uploader is active";

  if (m_glPagingTexturesDirty.size() < m_nChannels) {
    m_glPagingTexturesDirty.resize(m_nChannels, true);
  }
  if (m_channelPageDirectoryTextures.size() < m_nChannels) {
    m_channelPageDirectoryTextures.resize(m_nChannels);
  }
  if (m_channelPageTableCacheTextures.size() < m_nChannels) {
    m_channelPageTableCacheTextures.resize(m_nChannels);
  }
  if (m_channelImageCacheTextures.size() < m_nChannels) {
    m_channelImageCacheTextures.resize(m_nChannels);
  }

  CHECK_LT(c, m_channelPageDirectories.size());
  CHECK_LT(c, m_channelPageTableCaches.size());

  CHECK(m_pageDirectorySize.x > 0u && m_pageDirectorySize.y > 0u && m_pageDirectorySize.z > 0u) << m_pageDirectorySize;
  CHECK(m_pageTableCacheSize.x > 0u && m_pageTableCacheSize.y > 0u && m_pageTableCacheSize.z > 0u) << m_pageTableCacheSize;

  const glm::uvec3 imageBlockTotal = m_imageBlockSize + m_imageBlockSizePad;
  const glm::uvec3 imageCacheDimensions = m_imageCacheNumBlocks * imageBlockTotal;
  CHECK(imageCacheDimensions.x > 0u && imageCacheDimensions.y > 0u && imageCacheDimensions.z > 0u) << imageCacheDimensions;

  const bool shouldRecreate = m_glPagingTexturesDirty[c] != 0u;
  if (shouldRecreate) {
    m_channelPageDirectoryTextures[c].reset();
    m_channelPageTableCacheTextures[c].reset();
    m_channelImageCacheTextures[c].reset();
  }

  if (!m_channelPageDirectoryTextures[c] || m_channelPageDirectoryTextures[c]->dimension() != m_pageDirectorySize) {
    m_channelPageDirectoryTextures[c] = std::make_unique<Z3DTexture>(GL_TEXTURE_3D,
                                                                     GLint(GL_RGBA32UI),
                                                                     m_pageDirectorySize,
                                                                     GL_RGBA_INTEGER,
                                                                     GL_UNSIGNED_INT,
                                                                     m_channelPageDirectories[c].data(),
                                                                     GLint(GL_NEAREST),
                                                                     GLint(GL_NEAREST));
  }

  if (!m_channelPageTableCacheTextures[c] || m_channelPageTableCacheTextures[c]->dimension() != m_pageTableCacheSize) {
    m_channelPageTableCacheTextures[c] = std::make_unique<Z3DTexture>(GL_TEXTURE_3D,
                                                                      GLint(GL_RGBA32UI),
                                                                      m_pageTableCacheSize,
                                                                      GL_RGBA_INTEGER,
                                                                      GL_UNSIGNED_INT,
                                                                      m_channelPageTableCaches[c].data(),
                                                                      GLint(GL_NEAREST),
                                                                      GLint(GL_NEAREST));
  }

  if (!m_channelImageCacheTextures[c] || m_channelImageCacheTextures[c]->dimension() != imageCacheDimensions) {
    m_channelImageCacheTextures[c] = std::make_unique<Z3DTexture>(GLint(GL_R8),
                                                                  imageCacheDimensions,
                                                                  GL_RED,
                                                                  GL_UNSIGNED_BYTE,
                                                                  nullptr,
                                                                  GLint(GL_LINEAR),
                                                                  GLint(GL_LINEAR),
                                                                  GLint(GL_CLAMP_TO_BORDER));
    m_channelImageCacheTextures[c]->setBorderColor(glm::vec4(0.0f));
    m_channelImageCacheTextures[c]->clearImage();
  }

  m_glPagingTexturesDirty[c] = 0u;
}

void Z3DImg::rebuildGLPagingResources()
{
  // Backend switches may destroy and recreate the OpenGL context; drop any GL object
  // wrappers that cache names from a previous context (e.g. PBO).
  m_PBO.reset();

  // This method only applies to paged (downsampled) volumes. Non-downsampled images do not
  // allocate/initialize paging state (block sizes, cache dimensions), so rebuilding paging
  // resources would read uninitialized members and can crash.
  setVulkanImageBlockUploader(nullptr);
  if (!m_isVolumeDownsampled) {
    return;
  }

  // Recreate GL paging textures for all channels and reset CPU arrays to zero-filled state.
  // Page sizes and block counts are assumed already computed (constructor/setScale path).
  CHECK(m_pageDirectorySize.x > 0u && m_pageDirectorySize.y > 0u && m_pageDirectorySize.z > 0u)
    << "pageDirectorySize=" << m_pageDirectorySize;
  CHECK(m_pageTableCacheNumBlocks.x > 0u && m_pageTableCacheNumBlocks.y > 0u && m_pageTableCacheNumBlocks.z > 0u)
    << "pageTableCacheNumBlocks=" << m_pageTableCacheNumBlocks;
  CHECK(m_imageCacheNumBlocks.x > 0u && m_imageCacheNumBlocks.y > 0u && m_imageCacheNumBlocks.z > 0u)
    << "imageCacheNumBlocks=" << m_imageCacheNumBlocks;

  const auto imageBlockTotal = m_imageBlockSize + m_imageBlockSizePad;
  const glm::uvec3 imageCacheSize = m_imageCacheNumBlocks * imageBlockTotal;

  // Ensure vectors are sized correctly
  if (m_channelPageDirectories.size() != m_nChannels) {
    m_channelPageDirectories.resize(m_nChannels);
  }
  if (m_channelPageTableCaches.size() != m_nChannels) {
    m_channelPageTableCaches.resize(m_nChannels);
  }
  if (m_channelPageDirectoryTextures.size() != m_nChannels) {
    m_channelPageDirectoryTextures.resize(m_nChannels);
  }
  if (m_channelPageTableCacheTextures.size() != m_nChannels) {
    m_channelPageTableCacheTextures.resize(m_nChannels);
  }
  if (m_channelImageCacheTextures.size() != m_nChannels) {
    m_channelImageCacheTextures.resize(m_nChannels);
  }
  if (m_channelPageTableCacheManagers.size() != m_nChannels) {
    m_channelPageTableCacheManagers.resize(m_nChannels);
  }
  if (m_channelImageCacheManagers.size() != m_nChannels) {
    m_channelImageCacheManagers.resize(m_nChannels);
  }
  if (m_glPagingTexturesDirty.size() != m_nChannels) {
    m_glPagingTexturesDirty.assign(m_nChannels, 0u);
  } else {
    std::ranges::fill(m_glPagingTexturesDirty, 0u);
  }

  for (size_t c = 0; c < m_nChannels; ++c) {
    // Reset CPU arrays
    m_channelPageDirectories[c].resize(size_t(m_pageDirectorySize.x) * m_pageDirectorySize.y * m_pageDirectorySize.z);
    std::ranges::fill(m_channelPageDirectories[c], glm::uvec4(0));

    m_channelPageTableCaches[c].resize(size_t(m_pageTableCacheSize.x) * m_pageTableCacheSize.y *
                                       m_pageTableCacheSize.z);
    std::ranges::fill(m_channelPageTableCaches[c], glm::uvec4(0));

    // Reset cache managers
    m_channelPageTableCacheManagers[c] =
      std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, m_invalidKey);
    m_channelImageCacheManagers[c] =
      std::make_unique<Z3DBlockCache<glm::uvec4>>(imageBlockTotal, m_imageCacheNumBlocks, m_invalidKey);

    // Recreate GL textures
    m_channelPageDirectoryTextures[c] = std::make_unique<Z3DTexture>(GL_TEXTURE_3D,
                                                                     GLint(GL_RGBA32UI),
                                                                     m_pageDirectorySize,
                                                                     GL_RGBA_INTEGER,
                                                                     GL_UNSIGNED_INT,
                                                                     m_channelPageDirectories[c].data(),
                                                                     GLint(GL_NEAREST),
                                                                     GLint(GL_NEAREST));

    m_channelPageTableCacheTextures[c] = std::make_unique<Z3DTexture>(GL_TEXTURE_3D,
                                                                      GLint(GL_RGBA32UI),
                                                                      m_pageTableCacheSize,
                                                                      GL_RGBA_INTEGER,
                                                                      GL_UNSIGNED_INT,
                                                                      m_channelPageTableCaches[c].data(),
                                                                      GLint(GL_NEAREST),
                                                                      GLint(GL_NEAREST));

    m_channelImageCacheTextures[c] = std::make_unique<Z3DTexture>(GLint(GL_R8),
                                                                  imageCacheSize,
                                                                  GL_RED,
                                                                  GL_UNSIGNED_BYTE,
                                                                  nullptr,
                                                                  GLint(GL_LINEAR),
                                                                  GLint(GL_LINEAR),
                                                                  GLint(GL_CLAMP_TO_BORDER));
    m_channelImageCacheTextures[c]->setBorderColor(glm::vec4(0.0f));
    m_channelImageCacheTextures[c]->clearImage();
    m_glPagingTexturesDirty[c] = 0u;
  }
  VLOG(2) << "rebuildGLPagingResources finished";
}

} // namespace nim

template folly::coro::Task<void> nim::Z3DImg::readImageBlockToQueueAsync<
  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<nim::ZImg>, std::optional<std::string>>, true>>(
  size_t,
  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>&,
  size_t,
  const nim::ZImgInfo&,
  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<nim::ZImg>, std::optional<std::string>>, true>&) const;

template folly::coro::Task<void> nim::Z3DImg::readImageBlocksToQueueAsync<
  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<nim::ZImg>, std::optional<std::string>>, true>>(
  size_t,
  const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>&,
  const nim::ZImgInfo&,
  folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<nim::ZImg>, std::optional<std::string>>, true>&,
  nim::ZBenchTimer&) const;
