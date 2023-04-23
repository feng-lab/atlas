#include "z3dimg.h"

#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zkmeans.h"
#include "zbenchtimer.h"
#include "zcancellation.h"
#include "zcpuinfo.h"
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/MPMCQueue.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <algorithm>
#include <chrono>
#include <memory>

DEFINE_uint32(atlas_log_folly_global_executor_status_interval_in_seconds,
              5,
              "Interval in seconds for logging folly global executor status during waiting, default is 5");

DEFINE_uint32(atlas_number_of_blocks_to_use_PBO_threashold,
              0,
              "Use PBO when number of blocks to upload is larger than this threshold, default is 0");

DECLARE_double(atlas_image_cache_memory_proportion);
DECLARE_double(atlas_image_region_cache_memory_proportion);

namespace nim {

Z3DImg::Z3DImg(const ZImgPack& imgPack,
               const glm::vec3& scale,
               const std::vector<glm::dvec2>& displayRanges,
               QObject* parent)
  : QObject(parent)
  , m_imgPack(imgPack)
  , m_channelDisplayRanges(displayRanges)
{
  const ZImgInfo& info = m_imgPack.imgInfo();
  if (info.depth > 1) {
    m_widthScale = info.width <= 512_uz ? 1.0 : 512.0 / info.width;
    m_heightScale = info.height <= 512_uz ? 1.0 : 512.0 / info.height;
    m_depthScale = info.depth <= 512_uz ? 1.0 : 512.0 / info.depth;
  } else {
    Z3DGpuInfo::instance()
      .getDataScaleForTexture(info.width, info.height, info.depth, m_widthScale, m_heightScale, m_depthScale);
  }
  m_isVolumeDownsampled = m_widthScale != 1.0 || m_heightScale != 1.0 || m_depthScale != 1.0;

  m_volumeDimension = glm::uvec3(info.width * m_widthScale, info.height * m_heightScale, info.depth * m_depthScale);
  m_volumeSpacing = glm::vec3(1.f / m_widthScale, 1.f / m_heightScale, 1.f / m_depthScale);

  m_nChannels = m_imgPack.imgInfo().numChannels;

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
    auto imageBlockTotalSize = m_imageBlockSize + m_imageBlockSizePad;

    glm::uvec3 imageCacheSize;
    if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 32000) {
#ifdef Q_OS_MACOS
      imageCacheSize = glm::uvec3(2048, 2048, 2048); // 8G
#else
      imageCacheSize = glm::uvec3(2048, 2048, 2048); // 8G
      // imageCacheSize = glm::uvec3(3072, 2048, 2048); // 12G
#endif
      m_pageTableCacheSize = glm::uvec3(512, 512, 256); // 512*512*256*4*4   1073MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 20000) {
      imageCacheSize = glm::uvec3(2048, 2048, 2048); // 8G
      m_pageTableCacheSize = glm::uvec3(512, 512, 256); // 512*512*256*4*4   1073MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 16000) {
      imageCacheSize = glm::uvec3(2048, 2048, 1536); // 6G
      m_pageTableCacheSize = glm::uvec3(512, 256, 256); // 512*256*256*4*4   536MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 12000) {
      imageCacheSize = glm::uvec3(2048, 2048, 1024); // 4G
      m_pageTableCacheSize = glm::uvec3(512, 256, 256); // 512*256*256*4*4   536MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 8000) {
      imageCacheSize = glm::uvec3(2048, 1024, 1024); // 2G
      m_pageTableCacheSize = glm::uvec3(512, 256, 256); // 512*256*256*4*4   536MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 4000) {
      imageCacheSize = glm::uvec3(1024, 1024, 1024); // 1G
      m_pageTableCacheSize = glm::uvec3(256, 256, 256); // 256*256*256*4*4   268MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 2000) {
      imageCacheSize = glm::uvec3(1024, 1024, 512); // 0.5G
      m_pageTableCacheSize = glm::uvec3(256, 256, 128); // 256*256*128*4*4   134MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 1000) {
      imageCacheSize = glm::uvec3(1024, 1024, 256); // 0.25G
      m_pageTableCacheSize = glm::uvec3(256, 128, 128); // 256*128*128*4*4   67MB
    } else {
      imageCacheSize = glm::uvec3(1024, 1024, 128); // 0.125G
      m_pageTableCacheSize = glm::uvec3(128, 128, 128); // 128*128*128*4*4   34MB
    }
    m_imageCacheNumBlocks = imageCacheSize / imageBlockTotalSize;
    m_pageTableCacheNumBlocks = m_pageTableCacheSize / m_pageTableBlockSize;
    LOG(INFO) << "imageCacheNumBlocks: " << m_imageCacheNumBlocks
              << " pageTableCacheNumBlocks: " << m_pageTableCacheNumBlocks;

    m_channelPageTableCaches.resize(m_nChannels);
    for (size_t c = 0; c < m_nChannels; ++c) {
      m_channelPageTableCacheTextures.emplace_back(
        std::make_unique<Z3DTexture>(GLint(GL_RGBA32UI), m_pageTableCacheSize, GL_RGBA_INTEGER, GL_UNSIGNED_INT));
      m_channelPageTableCacheTextures[c]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));

      m_channelPageTableCaches[c].resize(m_channelPageTableCacheTextures[c]->numPixels());
      // m_channelPageTableCacheTextures[c]->uploadImage(m_channelPageTableCaches[c].data());  // will do this in
      // setScale

      m_channelImageCacheTextures.emplace_back(
        std::make_unique<Z3DTexture>(GLint(GL_R8),
                                     (m_imageBlockSize + m_imageBlockSizePad) * m_imageCacheNumBlocks,
                                     GL_RED,
                                     GL_UNSIGNED_BYTE));
      m_channelImageCacheTextures[c]->uploadImage();
    }

    setScale(scale);
    // setScale(glm::vec3(1,1,5));

    auto calculatedBlockUploadingBatchSize = std::round(ZCpuInfo::instance().nPhysicalRAM *
                                                        (1.0 - 0.2 - FLAGS_atlas_image_cache_memory_proportion -
                                                         FLAGS_atlas_image_region_cache_memory_proportion) /
                                                        (2.56 * 1024 * 1024 * 1024)) *
                                             100.;
    m_blockUploadingBatchSize = static_cast<size_t>(std::clamp(calculatedBlockUploadingBatchSize, 100., 32768.));
    LOG(INFO) << fmt::format("use block uploading batch size: {}", m_blockUploadingBatchSize);
  }
}

QString Z3DImg::samplerType() const
{
  if (is3DData()) {
    return "sampler3D";
  }

  return "sampler2D";
}

std::vector<std::unique_ptr<Z3DVolume>> Z3DImg::makeXSliceVolume(size_t x)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(x, x + 1, 0, -1, 0, -1, c, c + 1, 0, 1));
    croped.infoRef().width = m_imgPack.imgInfo().height;
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    } else {
      croped.normalize(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    }
    auto vh = std::make_unique<Z3DVolume>(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(std::move(vh));
  }
  return res;
}

std::vector<std::unique_ptr<Z3DVolume>> Z3DImg::makeYSliceVolume(size_t y)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0, -1, y, y + 1, 0, -1, c, c + 1, 0, 1));
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    } else {
      croped.normalize(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    }
    auto vh = std::make_unique<Z3DVolume>(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(std::move(vh));
  }
  return res;
}

std::vector<std::unique_ptr<Z3DVolume>> Z3DImg::makeZSliceVolume(size_t z)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0, -1, 0, -1, z, z + 1, c, c + 1, 0, 1));
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()), std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    } else {
      croped.normalize(m_channelDisplayRanges[c].x, m_channelDisplayRanges[c].y);
    }
    auto vh = std::make_unique<Z3DVolume>(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(std::move(vh));
  }
  return res;
}

void Z3DImg::setScale(const glm::vec3& scale)
{
  if (!m_isVolumeDownsampled) {
    return;
  }

  const ZImgInfo& info = m_imgPack.imgInfo();
  glm::dvec3 imgDim = glm::dvec3(info.width, info.height, info.depth);
  glm::dvec3 relativeResolution = glm::dvec3(glm::abs(scale));
  // make x and y scales same
  relativeResolution.x = std::max(relativeResolution.x, relativeResolution.y);
  relativeResolution.y = relativeResolution.x;

  double minRes = std::min(std::min(relativeResolution.x, relativeResolution.y), relativeResolution.z);
  relativeResolution /= minRes;
  imgDim *= relativeResolution;
  glm::dvec3 levels = glm::ceil(glm::log2(imgDim / glm::dvec3(m_imageBlockSize))) + 1.0;
  m_numLevels = std::max(std::max(levels.x, levels.y), levels.z);

  std::vector<size_t> sortedIndex = argSort(&relativeResolution[0], &relativeResolution[0] + 3);
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

  m_pageDirectorySize = glm::uvec3(0, 0, 0);
  m_levelScales.resize(m_numLevels);
  m_imageDimensions.resize(m_numLevels);
  m_imageBounds.resize(m_numLevels);
  m_pageTableDimensions.resize(m_numLevels);
  m_pageDirectoryDimensions.resize(m_numLevels);
  m_posToBlockIDs.resize(m_numLevels);
  m_pageDirectoryBases.resize(m_numLevels);
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

    m_imageDimensions[l] = glm::uvec3((info.width + m_levelScales[l].x - 1) / m_levelScales[l].x,
                                      (info.height + m_levelScales[l].y - 1) / m_levelScales[l].y,
                                      (info.depth + m_levelScales[l].z - 1) / m_levelScales[l].z);
    m_imageBounds[l] = m_imageDimensions[l] - 1_u32;
    m_pageTableDimensions[l] = (m_imageDimensions[l] + m_imageBlockSize - 1_u32) / m_imageBlockSize;
    m_pageDirectoryDimensions[l] = (m_pageTableDimensions[l] + m_pageTableBlockSize - 1_u32) / m_pageTableBlockSize;

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
      throw ZException(QString("Image (%1) is not supported").arg(info.toQString()));
    }
  }

  glm::uvec4 invalidKey(std::numeric_limits<uint32_t>::max());
  if (m_channelPageTableCacheManagers.size() != m_nChannels) {
    m_channelPageTableCacheManagers.resize(m_nChannels);
    m_channelImageCacheManagers.resize(m_nChannels);
    m_channelPageDirectoryTextures.resize(m_nChannels);
    m_channelPageDirectories.resize(m_nChannels);
  }
  for (size_t c = 0; c < m_nChannels; ++c) {
    m_channelPageTableCacheManagers[c] =
      std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, invalidKey);
    m_channelImageCacheManagers[c] = std::make_unique<Z3DBlockCache<glm::uvec4>>(m_imageBlockSize + m_imageBlockSizePad,
                                                                                 m_imageCacheNumBlocks,
                                                                                 invalidKey);

    // content of RGBA32I texture
    if (!m_channelPageDirectoryTextures[c] ||
        m_channelPageDirectoryTextures[c]->dimension() != glm::uvec3(m_pageDirectorySize)) {
      m_channelPageDirectoryTextures[c] = std::make_unique<Z3DTexture>(GL_TEXTURE_3D,
                                                                       GLint(GL_RGBA32UI),
                                                                       glm::uvec3(m_pageDirectorySize),
                                                                       GL_RGBA_INTEGER,
                                                                       GL_UNSIGNED_INT);
      m_channelPageDirectoryTextures[c]->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
    }

    m_channelPageDirectories[c].resize(m_channelPageDirectoryTextures[c]->numPixels());
    std::memset(m_channelPageDirectories[c].data(), 0, m_channelPageDirectories[c].size() * sizeof(glm::uvec4));
    m_channelPageDirectoryTextures[c]->uploadImage(m_channelPageDirectories[c].data());

    std::memset(m_channelPageTableCaches[c].data(), 0, m_channelPageTableCaches[c].size() * sizeof(glm::uvec4));
    m_channelPageTableCacheTextures[c]->uploadImage(m_channelPageTableCaches[c].data());
  }

  m_volumeVoxelWorldDimension = glm::abs(scale) * m_volumeSpacing;
  m_volumeVoxelWorldSize =
    std::max(std::max(m_volumeVoxelWorldDimension.x, m_volumeVoxelWorldDimension.y), m_volumeVoxelWorldDimension.z);

  m_voxelWorldDimensions.resize(m_numLevels);
  m_voxelWorldSizes.resize(m_numLevels);
  for (size_t l = 0; l < m_numLevels; ++l) {
    m_voxelWorldDimensions[l] = glm::abs(scale) * glm::vec3(m_levelScales[l]);
    m_voxelWorldSizes[l] =
      std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
    if (m_voxelWorldSizes[l] > m_volumeVoxelWorldSize) {
      m_numLevels = l + 1;
    }
  }

  for (size_t l = 0; l < m_numLevels; ++l) {
    LOG(INFO) << l << " pageDirectoryDimension: " << m_pageDirectoryDimensions[l]
              << " pageTableDimension: " << m_pageTableDimensions[l] << " imageDimension: " << m_imageDimensions[l]
              << " levelScale: " << m_levelScales[l] << " posToBlockID: " << m_posToBlockIDs[l]
              << " voxelWorldDimension: " << m_voxelWorldDimensions[l] << " voxelWorldSize: " << m_voxelWorldSizes[l];
  }
  LOG(INFO) << "volumeDimension: " << m_volumeDimension << " volumeVoxelWorldDimension: " << m_volumeVoxelWorldDimension
            << " volumeVoxelWorldSize: " << m_volumeVoxelWorldSize;

  CHECK(m_numLevels > 0);
  size_t l = m_numLevels - 1;
  m_imageDimensions[l] = m_volumeDimension;
  m_imageBounds[l] = m_imageDimensions[l] - 1_u32;

  LOG(INFO) << l << " pageDirectoryDimension: " << m_pageDirectoryDimensions[l]
            << " pageTableDimension: " << m_pageTableDimensions[l] << " imageDimension: " << m_imageDimensions[l]
            << " levelScale: " << m_levelScales[l] << " posToBlockID: " << m_posToBlockIDs[l]
            << " voxelWorldDimension: " << m_voxelWorldDimensions[l] << " voxelWorldSize: " << m_voxelWorldSizes[l];
}

void Z3DImg::setChannelDisplayRanges(const std::vector<glm::dvec2>& displayRanges)
{
  m_channelDisplayRanges = displayRanges;
  CHECK(m_imgPack.imgInfo().numChannels == m_channelDisplayRanges.size())
    << m_imgPack.imgInfo().numChannels << " " << m_channelDisplayRanges.size();
  readVolumes();

  if (m_isVolumeDownsampled) {
    glm::uvec4 invalidKey(std::numeric_limits<uint32_t>::max());
    for (size_t c = 0; c < m_nChannels; ++c) {
      m_channelPageTableCacheManagers[c] =
        std::make_unique<Z3DBlockCache<glm::uvec4>>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, invalidKey);
      m_channelImageCacheManagers[c] =
        std::make_unique<Z3DBlockCache<glm::uvec4>>(m_imageBlockSize + m_imageBlockSizePad,
                                                    m_imageCacheNumBlocks,
                                                    invalidKey);
      std::memset(m_channelPageDirectories[c].data(), 0, m_channelPageDirectories[c].size() * sizeof(glm::uvec4));
      m_channelPageDirectoryTextures[c]->uploadImage(m_channelPageDirectories[c].data());

      std::memset(m_channelPageTableCaches[c].data(), 0, m_channelPageTableCaches[c].size() * sizeof(glm::uvec4));
      m_channelPageTableCacheTextures[c]->uploadImage(m_channelPageTableCaches[c].data());
    }
  }
}

void Z3DImg::bindFullResBlockIDsShader(Z3DShaderProgram& shader, size_t c) const
{
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniformArray("pos_to_block_ids", m_posToBlockIDs.data(), m_numLevels);

  shader.bindTexture("page_directory", m_channelPageDirectoryTextures[c].get());
  shader.bindTexture("page_table_cache", m_channelPageTableCacheTextures[c].get());
}

void Z3DImg::bindFullResRenderShader(Z3DShaderProgram& shader, size_t c) const
{
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniform("image_address_to_normalized_texture_coord",
                    1.f / glm::vec3(m_channelImageCacheTextures[c]->dimension() - 1_u32));

  shader.bindTexture("page_directory", m_channelPageDirectoryTextures[c].get());
  shader.bindTexture("page_table_cache", m_channelPageTableCacheTextures[c].get());
  shader.bindTexture("image_cache", m_channelImageCacheTextures[c].get());
  shader.bindTexture("volume", m_volumes[c]->texture());
}

bool Z3DImg::updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                                size_t c,
                                                const folly::CancellationToken& cancellationToken)
{
  if (missingBlockIDs.empty()) {
    LOG(INFO) << "no missing blocks";
    return true;
  }
  auto numBlocksToRead = m_channelImageCacheManagers[c]->size();
  LOG(INFO) << "total " << m_channelImageCacheManagers[c]->size() << " need " << missingBlockIDs.size();

  ZBenchTimer bt("update page table");

  checkPageSystemError();

  processEventsAndMaybeCancel(cancellationToken);

  size_t count = 0;
  size_t alreadyMapped = 0;

  // pageDirectoryEntryKey, pageDirectoryEntry*, pageTableEntryKey
  std::vector<std::tuple<glm::uvec4, glm::uvec4*, glm::uvec4>> pendingBlocks;
  // go through all blocks and touch used blocks and collect pendingTasks if page table is already mapped (only need
  // image block)
  std::vector<std::tuple<glm::uvec4, glm::uvec4*>> pendingTasks; // pageTableEntryKey, pageTableEntry*
  // used to make sure used page table block will not be swapped out
  std::set<glm::uvec4, Vec4Compare<uint32_t, glm::highp>> usedPageDirectoryEntryKeys;

  auto imageBlockSize = m_imageBlockSize + m_imageBlockSizePad;
  size_t emptyBlockCount = 0;

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
    glm::uvec4 pageDirectoryEntryKey = pageTableEntryKey / glm::uvec4(m_pageTableBlockSize, 1);
    glm::uvec3 pageDirectoryEntryCoord = m_pageDirectoryBases[pageDirectoryEntryKey.w] + pageDirectoryEntryKey.xyz();
    glm::uvec4& pageDirectoryEntryRef =
      m_channelPageDirectories[c][pageDirectoryEntryCoord.z * m_pageDirectorySize.y * m_pageDirectorySize.x +
                                  pageDirectoryEntryCoord.y * m_pageDirectorySize.x + pageDirectoryEntryCoord.x];

    if (pageDirectoryEntryRef.w == m_emptyFlag) { // page table is empty block
      CHECK(false); // empty page table is not supported yet
      ++emptyBlockCount;
      ++count;
      continue;
    } else if (pageDirectoryEntryRef.w > 0) { // page table mapped
      auto pageTableEntryCoord = pageDirectoryEntryRef.xyz() + pageTableEntryKey.xyz() % m_pageTableBlockSize;
      auto pageTableEntryPtr =
        &m_channelPageTableCaches[c][pageTableEntryCoord.z * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                     pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x];

      if (pageTableEntryPtr->w != 0) { // image block already mapped or is empty block
        if (pageTableEntryPtr->w == m_emptyFlag) {
          CHECK(false) << *pageTableEntryPtr; // block id shader should not collect mapped empty block
          ++emptyBlockCount;
        } else {
          m_channelImageCacheManagers[c]->touch(pageTableEntryKey);
          ++alreadyMapped;
        }
        usedPageDirectoryEntryKeys.insert(pageDirectoryEntryKey);
        ++count;

        continue; // skip current image block and go to next
      }

      // page table mapped but image block is not mapped
      // increase pageDirectoryEntryRef now, upload image blocks and update page table block later in pendingTasks
      ++pageDirectoryEntryRef.w;
      if (isImageBlockEmpty(c, pageTableEntryKey, imageBlockSize)) {
        *pageTableEntryPtr = glm::uvec4(0, 0, 0, m_emptyFlag);
        ++emptyBlockCount;
      } else {
        // pageTableEntryPtr->w must be 0 here
        pendingTasks.push_back(std::make_tuple(pageTableEntryKey, pageTableEntryPtr));
      }
      ++count;
    } else { // pageDirectoryEntryRef.w == 0, page table not mapped
      pendingBlocks.push_back(std::make_tuple(pageDirectoryEntryKey, &pageDirectoryEntryRef, pageTableEntryKey));
    }
  }

  for (const auto& pageDirectoryEntryKey : usedPageDirectoryEntryKeys) {
    m_channelPageTableCacheManagers[c]->touch(pageDirectoryEntryKey);
  }
  auto numAvailablePageCacheBlock =
    index_t(m_channelPageTableCacheManagers[c]->size()) - index_t(usedPageDirectoryEntryKeys.size());
  clearAndDeallocate(usedPageDirectoryEntryKeys);

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
        insertPageTableBlockToCache(c, pageDirectoryEntryKey, *pageDirectoryEntryPtr);
        // after insertion, pageDirectoryEntryPtr->w == 1, will add a pendingTask
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
      *pageTableEntryPtr = glm::uvec4(0, 0, 0, m_emptyFlag);
      ++emptyBlockCount;
    } else {
      // pageTableEntryPtr->w must be 0 here
      CHECK(pageTableEntryPtr->w == 0) << *pageTableEntryPtr;
      pendingTasks.push_back(std::make_tuple(pageTableEntryKey, pageTableEntryPtr));
    }
    ++count;
  }
  clearAndDeallocate(pendingBlocks);

  //  for (size_t i = 0; i < pendingTasks.size(); ++i) {
  //    const auto& pageTableEntryKey = std::get<0>(pendingTasks[i]);
  //    glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
  //    auto imageBlockSize = m_imageBlockSize;
  //    LOG(INFO) << m_levelScales[blockImagePos.x].x * blockImagePos.y << " "
  //              << m_levelScales[blockImagePos.x].x * blockImagePos.z << " "
  //              << m_levelScales[blockImagePos.x].z * blockImagePos.w << " "
  //              << m_levelScales[blockImagePos.x].x * imageBlockSize.x << " "
  //              << m_levelScales[blockImagePos.x].x * imageBlockSize.y << " "
  //              << m_levelScales[blockImagePos.x].z * imageBlockSize.z;
  //  }

  size_t readEmptyBlockCount = 0;
  if (!pendingTasks.empty() || emptyBlockCount > 0) { // we have changed the cache system
    auto uploadGuard = folly::makeGuard([=]() {
      m_channelPageDirectoryTextures[c]->uploadImage(m_channelPageDirectories[c].data());
      m_channelPageTableCacheTextures[c]->uploadImage(m_channelPageTableCaches[c].data());
    });

    // read image
    if (!pendingTasks.empty()) {
      readEmptyBlockCount = readAndUploadImageBlocks(c, pendingTasks, cancellationToken);
    }
  }

  LOG(INFO) << fmt::format("filled {} blocks ({} already mapped, {} empty blocks, read {} blocks ({} empty))",
                           count,
                           alreadyMapped,
                           emptyBlockCount,
                           pendingTasks.size(),
                           readEmptyBlockCount);
  // glFinish();
  STOP_AND_LOG(bt)

  // checkPageSystemError();

  return count == missingBlockIDs.size();
}

// #define ATLAS_uploadImageCache_USE_MPMCQueue

void Z3DImg::readVolumes()
{
  m_volumes.clear();

  ZImg img = m_imgPack.resizedImg(m_volumeDimension.x, m_volumeDimension.y, m_volumeDimension.z, 0);

  if (m_nChannels == 1) {
    if (!img.isType<uint8_t>()) {
      img = img.convertTo<uint8_t>(m_channelDisplayRanges[0].x, m_channelDisplayRanges[0].y);
    } else if (img.validBitCount() != 0 && img.validBitCount() != 8 && img.validBitCount() != 16) {
      img.normalize(m_channelDisplayRanges[0].x, m_channelDisplayRanges[0].y);
    }

    auto vh = std::make_unique<Z3DVolume>(img, m_volumeSpacing, glm::vec3(.0));

    m_volumes.emplace_back(std::move(vh));
  } else {
    for (size_t i = 0; i < m_nChannels; ++i) {
      ZImg cImg = img.crop(ZImgRegion(0, -1, 0, -1, 0, -1, i, i + 1));
      if (!cImg.isType<uint8_t>()) {
        cImg = cImg.convertTo<uint8_t>(m_channelDisplayRanges[i].x, m_channelDisplayRanges[i].y);
      } else if (cImg.validBitCount() != 0 && cImg.validBitCount() != 8 && cImg.validBitCount() != 16) {
        cImg.normalize(m_channelDisplayRanges[i].x, m_channelDisplayRanges[i].y);
      }
      auto vh = std::make_unique<Z3DVolume>(cImg, m_volumeSpacing, glm::vec3(.0));

      m_volumes.emplace_back(std::move(vh));
    } // for each cannel
  }

  for (size_t i = 0; i < m_nChannels; ++i) {
    m_volumes[i]->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[i].r / 255.,
                                        m_imgPack.imgInfo().channelColors[i].g / 255.,
                                        m_imgPack.imgInfo().channelColors[i].b / 255.));
  }
}

void Z3DImg::insertPageTableBlockToCache(size_t c,
                                         const glm::uvec4& pageDirectoryEntryKey,
                                         glm::uvec4& pageDirectoryEntryRef)
{
  glm::uvec4 erasedKey;
  glm::uvec3 pageTableBlockCachePos = m_channelPageTableCacheManagers[c]->insert(pageDirectoryEntryKey, erasedKey);
  pageDirectoryEntryRef = glm::uvec4(pageTableBlockCachePos, 1);

  if (erasedKey.x != std::numeric_limits<uint32_t>::max()) {
    for (size_t z = 0; z < m_pageTableBlockSize.z; ++z) {
      for (size_t y = 0; y < m_pageTableBlockSize.y; ++y) {
        std::memset(
          &m_channelPageTableCaches[c]
                                   [(pageTableBlockCachePos.z + z) * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                    (pageTableBlockCachePos.y + y) * m_pageTableCacheSize.x + pageTableBlockCachePos.x],
          0,
          m_pageTableBlockSize.x * sizeof(glm::uvec4));
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

void Z3DImg::insertImageBlockToCache(size_t c, const glm::uvec4& pageTableEntryKey, glm::uvec4& pageTableEntryRef)
{
  glm::uvec4 erasedKey;
  glm::uvec3 imageBlockCachePos = m_channelImageCacheManagers[c]->insert(pageTableEntryKey, erasedKey);
  pageTableEntryRef = glm::uvec4(imageBlockCachePos, 1);
  // LOG(INFO) << blockKey << " " << erasedKey << " " << m_posToBlockIDs[level] << " " << blockID << " " <<
  // level;
  if (erasedKey.x != std::numeric_limits<uint32_t>::max()) { // valid
    glm::uvec4 erasedKeyPageDirectoryEntryKey = erasedKey / glm::uvec4(m_pageTableBlockSize, 1);
    glm::uvec3 erasedKeyPageDirectoryEntryCoord =
      m_pageDirectoryBases[erasedKeyPageDirectoryEntryKey.w] + erasedKeyPageDirectoryEntryKey.xyz();
    glm::uvec4& erasedKeyPageDirectoryEntry =
      m_channelPageDirectories[c][erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x +
                                  erasedKeyPageDirectoryEntryCoord.x];

    CHECK(erasedKeyPageDirectoryEntry.w != m_emptyFlag);
    if (erasedKeyPageDirectoryEntry.w > 0) {
      glm::uvec3 erasedKeyPageTableEntryCoord =
        erasedKeyPageDirectoryEntry.xyz() + erasedKey.xyz() % m_pageTableBlockSize;
      glm::uvec4& erasedKeyPageTableEntry =
        m_channelPageTableCaches[c][erasedKeyPageTableEntryCoord.z * m_pageTableCacheSize.y * m_pageTableCacheSize.x +
                                    erasedKeyPageTableEntryCoord.y * m_pageTableCacheSize.x +
                                    erasedKeyPageTableEntryCoord.x];
      CHECK(erasedKeyPageTableEntry.w != m_emptyFlag);
      if (erasedKeyPageTableEntry.w > 0) {
        erasedKeyPageTableEntry.w = 0;
        --erasedKeyPageDirectoryEntry.w;
      }
    }
  }
}

size_t Z3DImg::readAndUploadImageBlocks(size_t c,
                                        const std::vector<std::tuple<glm::uvec4, glm::uvec4*>>& pendingTasks,
                                        const folly::CancellationToken& cancellationToken)
{
  size_t emptyBlockCount = 0;

  ZBenchTimer bti(fmt::format("upload image ch{} cache", c));

  processEventsAndMaybeCancel(cancellationToken);

  LOG(INFO) << "reading " << pendingTasks.size() << " image blocks...";

  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto p = dynamic_cast<folly::CPUThreadPoolExecutor*>(cpuExecutor.get());
  CHECK(p);

  //  if (auto p = dynamic_cast<folly::CPUThreadPoolExecutor*>(cpuExecutor.get()); p) {
  //    LOG(INFO) << "number of priorities: " << static_cast<int>(p->getNumPriorities());
  //  }

  auto imageBlockSize = m_imageBlockSize + m_imageBlockSizePad;
  ZImgInfo resInfo(imageBlockSize.x, imageBlockSize.y, imageBlockSize.z, 1);
  auto blockSizeInByte = resInfo.byteNumber();

  uint8_t* pboPtr = nullptr;
  std::vector<uint8_t> pboLocalBuffer;
  if (pendingTasks.size() >= FLAGS_atlas_number_of_blocks_to_use_PBO_threashold) {
    m_PBO.bind(GL_PIXEL_UNPACK_BUFFER);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, blockSizeInByte * pendingTasks.size(), nullptr, GL_STREAM_DRAW);
    pboPtr = (uint8_t*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (!pboPtr) {
      LOG(WARNING) << "glMapBuffer failed on PBO";
    } else {
      pboLocalBuffer.resize(blockSizeInByte * pendingTasks.size(), 128);
    }
  }
  auto pboGuard = folly::makeGuard([=]() {
    m_PBO.release(GL_PIXEL_UNPACK_BUFFER);
  });

  if (!pboPtr) {
    processEventsAndMaybeCancel(cancellationToken);

    folly::UMPSCQueue<std::tuple<size_t, std::shared_ptr<ZImg>>, true> imgQueue;
    std::vector<folly::Future<folly::Unit>> blockFutures;
    blockFutures.reserve(pendingTasks.size());
    for (size_t i = 0; i < pendingTasks.size(); ++i) {
      const auto& pageTableEntryKey = std::get<0>(pendingTasks[i]);
      glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
      blockFutures.push_back(folly::via(cpuExecutor, [=, &imgQueue, &resInfo]() {
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
    auto f = folly::collect(blockFutures).via(cpuExecutor).thenValue([=](auto&&) {
      maybeCancel(cancellationToken);
      LOG(INFO) << "image blocks reading finished.";
    });

    processEventsAndMaybeCancel(cancellationToken);

    std::tuple<size_t, std::shared_ptr<ZImg>> elem;
    auto lastLogTime = std::chrono::steady_clock::now();
    int remainingBlocksToUpload = static_cast<int>(pendingTasks.size());
    while (remainingBlocksToUpload > 0) {
      processEventsAndMaybeCancel(cancellationToken);

      if (imgQueue.try_dequeue_until(
            elem,
            std::chrono::steady_clock::now() +
              std::chrono::seconds(FLAGS_atlas_log_folly_global_executor_status_interval_in_seconds))) {
        const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[std::get<0>(elem)];
        if (!std::get<1>(elem)) {
          ++emptyBlockCount;
          *pageTableEntryPtr = glm::uvec4(0, 0, 0, m_emptyFlag);
        } else {
          insertImageBlockToCache(c, pageTableEntryKey, *pageTableEntryPtr);
          m_channelImageCacheTextures[c]->uploadSubImage(pageTableEntryPtr->xyz(),
                                                         imageBlockSize,
                                                         std::get<1>(elem)->channelData(0));
        }
        --remainingBlocksToUpload;
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
  } else {
    // use PBO
    { // scope for glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      auto bufferGuard = folly::makeGuard([]() {
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      });

      processEventsAndMaybeCancel(cancellationToken);

      size_t batchSize = m_blockUploadingBatchSize;
      CHECK(batchSize > 0);

      size_t taskIdx = 0;
      while (taskIdx < pendingTasks.size()) {
        processEventsAndMaybeCancel(cancellationToken);

        size_t numberOfTasks = std::min(batchSize, pendingTasks.size() - taskIdx);
        std::vector<folly::Future<folly::Unit>> blockFutures;
        blockFutures.reserve(numberOfTasks);
        size_t finalTaskIdx = taskIdx + numberOfTasks;
        for (; taskIdx < finalTaskIdx; ++taskIdx) {
          const auto& pageTableEntryKey = std::get<0>(pendingTasks[taskIdx]);
          auto pageTableEntryPtr = std::get<1>(pendingTasks[taskIdx]);
          glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(m_imageBlockSize, 1);
          blockFutures.push_back(folly::via(cpuExecutor, [=, &resInfo, &pboLocalBuffer]() {
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
              .thenValueInline([=, &pboLocalBuffer](std::shared_ptr<ZImg>&& img) {
                maybeCancel(cancellationToken);
                if (!img) {
                  *pageTableEntryPtr = glm::uvec4(0, 0, 0, m_emptyFlag);
                } else {
                  memcpy(pboLocalBuffer.data() + taskIdx * blockSizeInByte, img->channelData(0), blockSizeInByte);
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
          processEventsAndMaybeCancel(cancellationToken);

          auto poolStats = p->getPoolStats();
          LOG(INFO) << fmt::format("pending/total task count: {}/{}, active/idle thread count: {}/{}",
                                   poolStats.pendingTaskCount,
                                   poolStats.totalTaskCount,
                                   poolStats.activeThreadCount,
                                   poolStats.idleThreadCount);
        }
      }

      processEventsAndMaybeCancel(cancellationToken);

      memcpy(pboPtr, pboLocalBuffer.data(), pboLocalBuffer.size());
      clearAndDeallocate(pboLocalBuffer);
      LOG(INFO) << "image blocks reading finished.";
    }

    ZBenchTimer bt_pboUpload("PBO uploading");
    for (size_t i = 0; i < pendingTasks.size(); ++i) {
      processEventsAndMaybeCancel(cancellationToken);

      const auto& [pageTableEntryKey, pageTableEntryPtr] = pendingTasks[i];
      if (pageTableEntryPtr->w == m_emptyFlag) {
        ++emptyBlockCount;
        continue;
      }
      insertImageBlockToCache(c, pageTableEntryKey, *pageTableEntryPtr);
      m_channelImageCacheTextures[c]->uploadSubImage(pageTableEntryPtr->xyz(),
                                                     imageBlockSize,
                                                     (const void*)(i * blockSizeInByte));
    }
    STOP_AND_LOG(bt_pboUpload)
  }
  STOP_AND_LOG(bti)

  return emptyBlockCount;
}

void Z3DImg::checkPageSystemError()
{
  for (size_t c = 0; c < m_nChannels; ++c) {
    for (size_t i = 0; i < m_channelPageDirectories[c].size(); ++i) {
      if (m_channelPageDirectories[c][i].w == 0) {
        continue;
      }
      CHECK(m_channelPageDirectories[c][i].w > 0);

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

      CHECK(level < m_numLevels);

      glm::uvec4 pageTableKey(pdLoc, level);
      CHECK(m_channelPageTableCacheManagers[c]->exists(pageTableKey));
      CHECK(m_channelPageTableCacheManagers[c]->get(pageTableKey) == m_channelPageDirectories[c][i].xyz());
      CHECK(glm::all(glm::lessThan(m_channelPageDirectories[c][i].xyz(), m_pageTableCacheSize)));

      uint32_t numValidEntry = 0;
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
                CHECK(m_channelImageCacheManagers[c]->exists(imageCacheKey));
                CHECK(m_channelImageCacheManagers[c]->get(imageCacheKey) == pageTableEntry.xyz());
                CHECK(glm::all(glm::lessThan(pageTableEntry.xyz(), m_channelImageCacheTextures[c]->dimension())));
              }
            }
          }
        }
      }
      CHECK(numValidEntry <= m_channelPageDirectories[c][i].w)
        << numValidEntry << " " << m_channelPageDirectories[c][i].w;
      if (numValidEntry != m_channelPageDirectories[c][i].w) {
        LOG(INFO) << "Fixing cancellation caused inconsistency";
        m_channelPageDirectories[c][i].w = numValidEntry;
      }
    }
  }
}

} // namespace nim
