#include "z3dimg.h"

#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zkmeans.h"
#include "zbenchtimer.h"
#include "zexception.h"
#include "zlog.h"
#include <QApplication>
#include <QMessageBox>
#include <boost/functional/hash.hpp>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_unordered_set.h>
#include <algorithm>
#include <memory>

namespace nim {

Z3DImg::Z3DImg(const ZImgPack& imgPack, const glm::vec3& scale, QObject* parent)
  : QObject(parent)
  , m_imgPack(imgPack)
  , m_isVolumeDownsampled(false)
{
  readVolumes();

  if (m_isVolumeDownsampled) {
    m_channelPendingUpdates.resize(m_nChannels);

    auto imageBlockTotalSize = m_imageBlockSize + m_imageBlockSizePad;

    glm::uvec3 imageCacheSize;
    // m_imageBlockReadSize = glm::ivec3(510, 510, 30);
    if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 20480) {
#ifdef Q_OS_MACOS
      imageCacheSize = glm::uvec3(2048, 2048, 2048); // 8G
#else
      imageCacheSize = glm::uvec3(4096, 2048, 2048); // 16G
#endif
      m_pageTableCacheSize = glm::uvec3(512, 512, 256); // 512*512*256*4*4   1073MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 8192) {
#ifdef Q_OS_MACOS
      imageCacheSize = glm::uvec3(2048, 1024, 1024); // 2G
#else
      imageCacheSize = glm::uvec3(2048, 2048, 1536); // 6G
#endif
      m_pageTableCacheSize = glm::uvec3(512, 256, 256); // 512*256*256*4*4   536MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 4096) {
      imageCacheSize = glm::uvec3(2048, 1024, 1024); // 2G
      m_pageTableCacheSize = glm::uvec3(256, 256, 256); // 256*256*256*4*4   268MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 2048) {
      imageCacheSize = glm::uvec3(1024, 1024, 512); // 0.5G
      m_pageTableCacheSize = glm::uvec3(256, 256, 128); // 256*256*128*4*4   134MB
    } else if (Z3DGpuInfo::instance().dedicatedVideoMemoryMB() >= 1024) {
      imageCacheSize = glm::uvec3(1024, 1024, 256); // 0.25G
      m_pageTableCacheSize = glm::uvec3(256, 128, 128); // 256*128*128*4*4   67MB
    } else {
      imageCacheSize = glm::uvec3(1024, 1024, 128); // 0.125G
      m_pageTableCacheSize = glm::uvec3(128, 128, 128); // 128*128*128*4*4   34MB
    }
    m_imageCacheNumBlocks = imageCacheSize / imageBlockTotalSize;
    m_pageTableCacheNumBlocks = m_pageTableCacheSize / m_pageTableBlockSize;

    m_pageTableCacheTexture = std::make_unique<Z3DTexture>(
      GLint(GL_RGBA32UI), m_pageTableCacheSize, GL_RGBA_INTEGER, GL_UNSIGNED_INT);
    m_pageTableCache.resize(m_pageTableCacheTexture->numPixels(), glm::uvec4(0, 0, 0, m_unmappedFlag));
    m_pageTableCacheTexture->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
    m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

    for (size_t c = 0; c < m_imgPack.imgInfo().numChannels; ++c) {
      m_imageCacheTextures.emplace_back(
        new Z3DTexture(GLint(GL_R8), (m_imageBlockSize + m_imageBlockSizePad) * m_imageCacheNumBlocks,
                       GL_RED, GL_UNSIGNED_BYTE));
      m_imageCacheTextures[c]->uploadImage();
    }

    setScale(scale);
    //setScale(glm::vec3(1,1,5));
  }
}

QString Z3DImg::samplerType() const
{
  if (is3DData()) {
    return "sampler3D";
  }

  return "sampler2D";
}

std::vector<std::unique_ptr<Z3DVolume> > Z3DImg::makeXSliceVolume(size_t x)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(x, x + 1, 0, -1, 0, -1, c, c + 1, 0, 1));
    croped.infoRef().width = m_imgPack.imgInfo().height;
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    } else {
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    }
    auto vh = new Z3DVolume(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(vh);
  }
  return res;
}

std::vector<std::unique_ptr<Z3DVolume> > Z3DImg::makeYSliceVolume(size_t y)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0, -1, y, y + 1, 0, -1, c, c + 1, 0, 1));
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    } else {
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    }
    auto vh = new Z3DVolume(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(vh);
  }
  return res;
}

std::vector<std::unique_ptr<Z3DVolume> > Z3DImg::makeZSliceVolume(size_t z)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfo::instance().maxTextureSize();
  for (size_t c = 0; c < m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0, -1, 0, -1, z, z + 1, c, c + 1, 0, 1));
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>()) {
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    } else {
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    }
    auto vh = new Z3DVolume(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(vh);
  }
  return res;
}

void Z3DImg::setScale(const glm::vec3& scale)
{
  if (!m_isVolumeDownsampled) {
    return;
  }

  glm::uvec4 invalidKey(std::numeric_limits<uint32_t>::max());

  m_pageTableCacheManager = std::make_unique<Z3DBlockCache<glm::uvec4>>(
    m_pageTableBlockSize, m_pageTableCacheNumBlocks, invalidKey);
  m_imageCacheManager = std::make_unique<Z3DBlockCache<glm::uvec4>>(
    m_imageBlockSize + m_imageBlockSizePad, m_imageCacheNumBlocks, invalidKey);
  for (auto& pu : m_channelPendingUpdates) {
    pu.clear();
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
    m_posToBlockIDs[l] = glm::uvec4(1,
                                    m_pageTableDimensions[l].x,
                                    m_pageTableDimensions[l].x * m_pageTableDimensions[l].y,
                                    l == 0 ? 1 : (m_posToBlockIDs[l - 1].w +
                                                  m_pageTableDimensions[l - 1].x * m_pageTableDimensions[l - 1].y *
                                                  m_pageTableDimensions[l - 1].z));
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
      throw ZGLException(QString("Image (%1) is not supported").arg(info.toQString()));
    }
    LOG(INFO) << l << " "
              << m_pageDirectoryDimensions[l] << " "
              << m_pageTableDimensions[l] << " "
              << m_imageDimensions[l] << " "
              << m_levelScales[l] << " "
              << m_posToBlockIDs[l];
  }

  // content of RGBA32I texture
  m_pageDirectoryTexture = std::make_unique<Z3DTexture>(
    GL_TEXTURE_3D, GLint(GL_RGBA32UI), glm::uvec3(m_pageDirectorySize), GL_RGBA_INTEGER, GL_UNSIGNED_INT);
  m_pageDirectory.resize(m_pageDirectoryTexture->numPixels());
  std::memset(m_pageDirectory.data(), 0, m_pageDirectory.size() * sizeof(glm::uvec4));
  m_pageDirectoryTexture->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());

  std::memset(m_pageTableCache.data(), 0, m_pageTableCache.size() * sizeof(glm::uvec4));
  m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

  m_voxelWorldDimensions.resize(m_numLevels);
  m_voxelWorldSizes.resize(m_numLevels);
  for (size_t l = 0; l < m_numLevels; ++l) {
    m_voxelWorldDimensions[l] = glm::abs(scale) * glm::vec3(m_levelScales[l]);
    m_voxelWorldSizes[l] = std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y),
                                    m_voxelWorldDimensions[l].z);
  }
}

void Z3DImg::bindFullResBlockIDsShader(Z3DShaderProgram& shader) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniformArray("pos_to_block_ids", m_posToBlockIDs.data(), m_numLevels);
}

void Z3DImg::bindFullResRenderShader(Z3DShaderProgram& shader) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", m_pageTableBlockSize);
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", m_imageBlockSize);
  shader.setUniform("image_address_to_normalized_texture_coord",
                    1.f / glm::vec3(m_imageCacheTextures[0]->dimension() - 1_u32));
}

void Z3DImg::bindImageCacheToFullResRenderShader(Z3DShaderProgram& shader, size_t c) const
{
  shader.bindTexture("image_cache", m_imageCacheTextures[c].get());
}

bool Z3DImg::updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                                const std::vector<uint32_t>& usedBlockIDs,
                                                bool silenceExistingWarning)
{
  auto numBlocksToRead = int(m_imageCacheManager->size()) - int(usedBlockIDs.size());
  if (silenceExistingWarning) {
    CHECK(usedBlockIDs.empty());
    LOG(INFO) << "total " << m_imageCacheManager->size()
              << " need " << missingBlockIDs.size();
  } else {
    LOG(INFO) << "total " << m_imageCacheManager->size()
              << " reuse " << usedBlockIDs.size()
              << " missing " << missingBlockIDs.size()
              << " will upload " << std::min<int>(missingBlockIDs.size(), numBlocksToRead);
  }
  if (missingBlockIDs.empty() || numBlocksToRead <= 0) {
    return false;
  }

  ZBenchTimer bt("update page table");
  bt.start();

  std::set<glm::uvec4, Vec4Compare<uint32_t, glm::highp>> usedPageTableKeys;
  size_t level = 0;
  for (auto blockID : usedBlockIDs) {  // blockID must be ordered, can not use unordered_set here
    while (level + 1 < m_numLevels && blockID >= m_posToBlockIDs[level + 1].w) {
      ++level;
    }

    blockID -= m_posToBlockIDs[level].w;
    auto z = blockID / m_posToBlockIDs[level].z;
    blockID -= z * m_posToBlockIDs[level].z;
    auto y = blockID / m_posToBlockIDs[level].y;
    blockID -= y * m_posToBlockIDs[level].y;
    glm::uvec4 blockKey(level, blockID, y, z);
    if (!glm::all(glm::lessThan(blockKey.yzw(), m_pageTableDimensions[level]))) {
      LOG(FATAL) << blockID << " " << blockKey << " " << m_pageTableDimensions[level];
    }
    usedPageTableKeys.insert(blockKey / glm::uvec4(1, m_pageTableBlockSize));
    m_imageCacheManager->touch(blockKey);
  }
  for (const auto& key : usedPageTableKeys) {
    m_pageTableCacheManager->touch(key);
  }

  auto count = 0;
  auto alreadyMapped = 0;
  // level = 0;
  glm::uvec4 erasedKey;
  auto numAvailablePageCacheBlock = index_t(m_pageTableCacheManager->size()) - index_t(usedPageTableKeys.size());
  CHECK(numAvailablePageCacheBlock >= 0);
  for (auto blockID : missingBlockIDs) {
    if (count >= numBlocksToRead) {
      break;
    }

    level = 0;
    while (level + 1 < m_numLevels && blockID >= m_posToBlockIDs[level + 1].w) {
      ++level;
    }

    glm::uvec4 pageTableEntryKey(level, blockID, 0, 0);
    pageTableEntryKey.y -= m_posToBlockIDs[level].w;
    pageTableEntryKey.w = pageTableEntryKey.y / m_posToBlockIDs[level].z;
    pageTableEntryKey.y -= pageTableEntryKey.w * m_posToBlockIDs[level].z;
    pageTableEntryKey.z = pageTableEntryKey.y / m_posToBlockIDs[level].y;
    pageTableEntryKey.y -= pageTableEntryKey.z * m_posToBlockIDs[level].y;
    if (!glm::all(glm::lessThan(pageTableEntryKey.yzw(), m_pageTableDimensions[level]))) {
      LOG(FATAL) << blockID << " " << pageTableEntryKey << " " << m_pageTableDimensions[level];
    }
    glm::uvec4 pageDirectoryEntryKey = pageTableEntryKey / glm::uvec4(1, m_pageTableBlockSize);
    glm::uvec3 pageDirectoryEntryCoord = m_pageDirectoryBases[pageDirectoryEntryKey.x] + pageDirectoryEntryKey.yzw();
    glm::uvec4& pageDirectoryEntry = m_pageDirectory[
      pageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
      pageDirectoryEntryCoord.y * m_pageDirectorySize.x + pageDirectoryEntryCoord.x];
    glm::uvec3 pageTableEntryCoord;

    if (pageDirectoryEntry.w > 0) {
      pageTableEntryCoord = pageDirectoryEntry.xyz() + pageTableEntryKey.yzw() % m_pageTableBlockSize;
      if (m_pageTableCache[pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
                           pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x].w != 0) {
        if (silenceExistingWarning) {
          m_imageCacheManager->touch(pageTableEntryKey);
          m_pageTableCacheManager->touch(pageDirectoryEntryKey);
          ++alreadyMapped;
          ++count;
        } else {
          LOG(ERROR) << "missing block is already mapped! " << pageTableEntryKey << " " << pageDirectoryEntryKey;
        }
        continue;
      }
    }

    glm::uvec3 imageBlockCachePos = m_imageCacheManager->insert(pageTableEntryKey, erasedKey);
    //LOG(INFO) << blockKey << " " << erasedKey << " " << m_posToBlockIDs[level] << " " << blockID << " " << level;
    if (erasedKey.x != std::numeric_limits<uint32_t>::max()) { //valid
      glm::uvec4 erasedKeyPageDirectoryEntryKey = erasedKey / glm::uvec4(1, m_pageTableBlockSize);
      glm::uvec3 erasedKeyPageDirectoryEntryCoord =
        m_pageDirectoryBases[erasedKeyPageDirectoryEntryKey.x] + erasedKeyPageDirectoryEntryKey.yzw();
      glm::uvec4& erasedKeyPageDirectoryEntry = m_pageDirectory[
        erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
        erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x + erasedKeyPageDirectoryEntryCoord.x];

      if (erasedKeyPageDirectoryEntry.w > 0) {
        glm::uvec3 erasedKeyPageTableEntryCoord =
          erasedKeyPageDirectoryEntry.xyz() + erasedKey.yzw() % m_pageTableBlockSize;
        glm::uvec4& erasedKeyPageTableEntry = m_pageTableCache[
          erasedKeyPageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          erasedKeyPageTableEntryCoord.y * m_pageTableCacheSize.x + erasedKeyPageTableEntryCoord.x];
        if (erasedKeyPageTableEntry.w > 0) {
          erasedKeyPageTableEntry.w = 0;
          --erasedKeyPageDirectoryEntry.w;
          if (erasedKeyPageDirectoryEntry.w == 0) {
            // unmap entire page table block
            m_pageTableCacheManager->remove(erasedKeyPageDirectoryEntryKey);
            ++numAvailablePageCacheBlock;
          }
        }
      }
    }

    if (pageDirectoryEntry.w == 0) { // page directory unmapped
      if (numAvailablePageCacheBlock > 0) { // construct new page table block
        glm::uvec3 pageTableBlockCachePos = m_pageTableCacheManager->insert(pageDirectoryEntryKey, erasedKey);
        pageDirectoryEntry = glm::uvec4(pageTableBlockCachePos, 1);

        if (erasedKey.x != std::numeric_limits<uint32_t>::max()) {
          for (size_t z = 0; z < m_pageTableBlockSize.z; ++z) {
            for (size_t y = 0; y < m_pageTableBlockSize.y; ++y) {
              std::memset(
                &m_pageTableCache[(pageTableBlockCachePos.z + z) * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
                                  (pageTableBlockCachePos.y + y) * m_pageTableCacheSize.x + pageTableBlockCachePos.x],
                0,
                m_pageTableBlockSize.x * sizeof(glm::ivec4));
            }
          }

          glm::uvec3 erasedKeyPageDirectoryEntryCoord = m_pageDirectoryBases[erasedKey.x] + erasedKey.yzw();
          glm::uvec4& erasedKeyPageDirectoryEntry = m_pageDirectory[
            erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
            erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x + erasedKeyPageDirectoryEntryCoord.x];
          erasedKeyPageDirectoryEntry.w = 0;
        }

        pageTableEntryCoord = pageDirectoryEntry.xyz() + pageTableEntryKey.yzw() % m_pageTableBlockSize;
        glm::uvec4& pageTableEntry = m_pageTableCache[
          pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x];
        pageTableEntry = glm::uvec4(imageBlockCachePos, 1);
        --numAvailablePageCacheBlock;
      } else {
        m_imageCacheManager->popFront();
        LOG(ERROR) << "no space for new page table block, skip current image block";
        continue;
      }
    } else { // page directory mapped
      CHECK(pageDirectoryEntry.w > 0);
      m_pageTableCache[pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
                       pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x] = glm::uvec4(
        imageBlockCachePos, 1);
      ++pageDirectoryEntry.w;
    }

    glm::uvec4 blockImagePos = pageTableEntryKey * glm::uvec4(1, glm::ivec3(m_imageBlockSize));
    auto blockCachePos = glm::uvec3(imageBlockCachePos);
    for (auto& pu : m_channelPendingUpdates) {
      if (auto it = std::find_if(pu.begin(), pu.end(), [&](const auto& pr) { return pr.first == blockCachePos; });
        it != pu.end()) {
        pu.erase(it);
      }
      pu.emplace_back(blockCachePos, blockImagePos);
    }
    ++count;
  }
  LOG(INFO) << "filled " << count << " blocks (" << alreadyMapped << " already mapped)";
  m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());
  m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());
  //glFinish();
  STOP_AND_LOG(bt)

  //checkPageSystemError();

  return count == int(missingBlockIDs.size());
}

void Z3DImg::uploadImageCache(size_t channel)
{
  if (m_channelPendingUpdates[channel].empty()) {
    return;
  }

  ZBenchTimer bt(fmt::format("upload image ch{} cache", channel));
  bt.start();

  //m_imgPack.stopCacheEviction();
  LOG(INFO) << "reading " << m_channelPendingUpdates[channel].size() << " image blocks...";

  ZBenchTimer bt_cc(fmt::format("collect reading cache keys for image ch{}", channel));
  bt_cc.start();
  tbb::concurrent_unordered_set<ZImgPack::HashKeyType, boost::hash<ZImgPack::HashKeyType>> ccKeySet;
  tbb::parallel_for(tbb::blocked_range<size_t>(0, m_channelPendingUpdates[channel].size()),
                    [&](const tbb::blocked_range<size_t>& r) {
                      for (auto i = r.begin(); i != r.end(); ++i) {
                        const auto& blockImagePos = m_channelPendingUpdates[channel][i].second;
                        auto keys = m_imgPack.collectCacheKeysForReadRegionToImg(
                          m_levelScales[blockImagePos.x].x,
                          m_levelScales[blockImagePos.x].z,
                          index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.x) / 2,
                          index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.y) / 2,
                          index_t(blockImagePos.w) - index_t(m_imageBlockSizePad.z) / 2,
                          m_imageBlockSize.x + m_imageBlockSizePad.x,
                          m_imageBlockSize.y + m_imageBlockSizePad.y,
                          m_imageBlockSize.z + m_imageBlockSizePad.z,
                          0,
                          true);
                        ccKeySet.insert(keys.begin(), keys.end());
                      }
                    }
  );
  STOP_AND_LOG(bt_cc)

  ZBenchTimer bt_preload(fmt::format("preload cache keys for image ch{}", channel));
  bt_preload.start();
  std::vector<ZImgPack::HashKeyType> missingCacheKeys;
  missingCacheKeys.reserve(ccKeySet.size());
  missingCacheKeys.insert(missingCacheKeys.end(), ccKeySet.begin(), ccKeySet.end());
  LOG(INFO) << "preloading " << missingCacheKeys.size() << " image pieces...";
  tbb::parallel_for(tbb::blocked_range<size_t>(0, missingCacheKeys.size()),
                    [&](const tbb::blocked_range<size_t>& r) {
                      for (auto i = r.begin(); i != r.end(); ++i) { m_imgPack.preLoadImageCache(missingCacheKeys[i]);
                      }
                    }
  );
  LOG(INFO) << "image cache size: " << m_imgPack.imageCacheSize();
  STOP_AND_LOG(bt_preload)

  ZBenchTimer bt_read(fmt::format("reading/assembling image blocks for image ch{}", channel));
  bt_read.start();
  std::vector<ZImg> imgs(m_channelPendingUpdates[channel].size());
  tbb::parallel_for(tbb::blocked_range<size_t>(0, imgs.size()),
                    [&](const tbb::blocked_range<size_t>& r) {
                      for (auto i = r.begin(); i != r.end(); ++i) {
                        const auto& blockImagePos = m_channelPendingUpdates[channel][i].second;
                        imgs[i] = ZImg(ZImgInfo(m_imageBlockSize.x + m_imageBlockSizePad.x,
                                                m_imageBlockSize.y + m_imageBlockSizePad.y,
                                                m_imageBlockSize.z + m_imageBlockSizePad.z,
                                                1));
                        m_imgPack.readRegionToImg(m_levelScales[blockImagePos.x].x, m_levelScales[blockImagePos.x].z,
                                                  index_t(blockImagePos.y) - index_t(m_imageBlockSizePad.x) / 2,
                                                  index_t(blockImagePos.z) - index_t(m_imageBlockSizePad.y) / 2,
                                                  index_t(blockImagePos.w) - index_t(m_imageBlockSizePad.z) / 2,
                                                  channel, 0, imgs[i]);
                      }
                    }
  );
  STOP_AND_LOG(bt_read)

  //m_imgPack.resumeCacheEviction();

  ZBenchTimer bt_upload(fmt::format("uploading image blocks to GPU for image ch{}", channel));
  bt_upload.start();
  for (size_t i = 0; i < imgs.size(); ++i) {
    m_imageCacheTextures[channel]->uploadSubImage(m_channelPendingUpdates[channel][i].first,
                                                  m_imageBlockSize + m_imageBlockSizePad, imgs[i].channelData(0));
  }

  m_channelPendingUpdates[channel].clear();
  //glFinish();
  STOP_AND_LOG(bt_upload)

  STOP_AND_LOG(bt)
}

void Z3DImg::readVolumes()
{
  m_volumes.clear();
  const ZImgInfo& info = m_imgPack.imgInfo();
  m_nChannels = info.numChannels;

#if 0
  // shader limit is 20 channels
  // limited by Max FS Texture Image Units
  // see https://www.opengl.org/wiki/Shader#Resource_limitations
  size_t maxPossibleChannels = std::min(20, (Z3DGpuInfoInstance.maxTextureImageUnits() - 4) / 2);
#else
  size_t maxPossibleChannels = Z3DGpuInfo::instance().maxArrayTextureLayers();
#endif
  if (m_nChannels > maxPossibleChannels) {
    QMessageBox::warning(QApplication::activeWindow(), QApplication::applicationName(),
                         QString("Due to hardware limit, only first %1 channels of this image will be shown").arg(
                           maxPossibleChannels));
    m_nChannels = maxPossibleChannels;
  }

  double widthScale = 1.0;
  double heightScale = 1.0;
  double depthScale = 1.0;
  Z3DGpuInfo::instance().getDataScaleForTexture(info.width, info.height, info.depth, widthScale, heightScale,
                                                depthScale);

  if (widthScale != 1.0 || heightScale != 1.0 || depthScale != 1.0) {
    m_isVolumeDownsampled = true;

    if (m_imgPack.imgInfo().depth > 1) {
      widthScale = info.width <= 512_uz ? 1.0 : 512.0 / info.width;
      heightScale = info.height <= 512_uz ? 1.0 : 512.0 / info.height;
      depthScale = info.depth <= 512_uz ? 1.0 : 512.0 / info.depth;
    }

    //return;
  }

  ZImg img = m_imgPack.resizedImg(info.width * widthScale,
                                  info.height * heightScale,
                                  info.depth * depthScale,
                                  0);
  if (!img.isType<uint8_t>()) {
    img = img.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
  } else if (img.validBitCount() != 0 && img.validBitCount() != 8 && img.validBitCount() != 16) {
    img.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
  }
  if (m_nChannels == 1) {
    auto vh = new Z3DVolume(img,
                            glm::vec3(1.f / widthScale, 1.f / heightScale, 1.f / depthScale),
                            glm::vec3(.0));

    m_volumes.emplace_back(vh);
  } else {
    for (size_t i = 0; i < m_nChannels; ++i) {
      ZImg cImg = img.crop(ZImgRegion(0, -1, 0, -1, 0, -1, i, i + 1));
      auto vh = new Z3DVolume(cImg,
                              glm::vec3(1.f / widthScale, 1.f / heightScale, 1.f / depthScale),
                              glm::vec3(.0));

      m_volumes.emplace_back(vh);
    } //for each cannel
  }

  for (size_t i = 0; i < m_nChannels; ++i) {
    m_volumes[i]->setVolColor(glm::vec3(info.channelColors[i].r / 255.,
                                        info.channelColors[i].g / 255.,
                                        info.channelColors[i].b / 255.));
  }
}

void Z3DImg::checkPageSystemError()
{
  for (size_t i = 0; i < m_pageDirectory.size(); ++i) {
    if (m_pageDirectory[i].w == 0) {
      continue;
    }
    CHECK(m_pageDirectory[i].w > 0);

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

    CHECK(level < 10000);

    glm::uvec4 pageTableKey(level, pdLoc);
    CHECK(m_pageTableCacheManager->exists(pageTableKey));
    CHECK(m_pageTableCacheManager->get(pageTableKey) == m_pageDirectory[i].xyz());
    CHECK(glm::all(glm::lessThan(m_pageDirectory[i].xyz(), m_pageTableCacheSize)));

    uint32_t numValidEntry = 0;
    for (size_t z = 0; z < m_pageTableBlockSize.z; ++z) {
      for (size_t y = 0; y < m_pageTableBlockSize.y; ++y) {
        for (size_t x = 0; x < m_pageTableBlockSize.x; ++x) {
          glm::uvec4 pageTableEntry = m_pageTableCache[
            (m_pageDirectory[i].z + z) * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
            (m_pageDirectory[i].y + y) * m_pageTableCacheSize.x + m_pageDirectory[i].x + x];
          if (pageTableEntry.w > 0) {
            ++numValidEntry;
            glm::uvec4 imageCacheKey(level, glm::uvec3(x, y, z) + pdLoc * m_pageTableBlockSize);
            CHECK(m_imageCacheManager->exists(imageCacheKey));
            CHECK(m_imageCacheManager->get(imageCacheKey) == pageTableEntry.xyz());
            CHECK(glm::all(glm::lessThan(pageTableEntry.xyz(), m_imageCacheTextures[0]->dimension())));
          }
        }
      }
    }
    CHECK(numValidEntry == m_pageDirectory[i].w);
  }
}

} // namespace nim


