#include "z3dimg.h"
#include <algorithm>
#include <QString>
#include "zlog.h"
#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zkmeans.h"
#include "zexception.h"
#include <QApplication>
#include <QMessageBox>
#include "zbenchtimer.h"

namespace nim {

Z3DImg::Z3DImg(const ZImgPack &imgPack, const glm::vec3 &scale, QObject *parent)
  : QObject(parent)
  , m_imgPack(imgPack)
  , m_isVolumeDownsampled(false)
{
  readVolumes();

  if (m_isVolumeDownsampled) {
    m_channelPendingUpdates.resize(m_nChannels);
#if 0
    m_pageTableBlockSize = glm::uvec3(32, 32, 32);
    m_imageBlockSize = glm::uvec3(64, 64, 64);
    m_imageBlockReadSize = glm::ivec3(512, 512, 64);
    if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 4096) {
      //m_imageCacheNumBlocks = glm::uvec3(16,16,16); // 1G
      m_imageCacheNumBlocks = glm::uvec3(15,15,16); // 1G
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 2); // 128*128*64*4*4   16MB
    } else if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 2048) {
      //m_imageCacheNumBlocks = glm::uvec3(16,16,8);
      m_imageCacheNumBlocks = glm::uvec3(15,15,8);
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 1); // 128*128*32*4*4   8MB
    } else if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 1024) {
      //m_imageCacheNumBlocks = glm::uvec3(16,16,4);
      m_imageCacheNumBlocks = glm::uvec3(15,15,4);
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 1); // 128*128*32*4*4   8MB
    } else {
      //m_imageCacheNumBlocks = glm::uvec3(16,16,2);
      m_imageCacheNumBlocks = glm::uvec3(15,15,2);
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 1); // 128*128*32*4*4   8MB
    }
#else
    m_pageTableBlockSize = glm::uvec3(32, 32, 32);
    m_imageBlockSize = imageBlockSize();
    m_imageBlockReadSize = glm::ivec3(510, 510, 30);
    if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 4096) {
      m_imageCacheNumBlocks = glm::uvec3(32,32,32); // 1G
      m_pageTableCacheNumBlocks = glm::uvec3(8, 8, 2); // 256*256*64*4*4   64MB
    } else if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 2048) {
      m_imageCacheNumBlocks = glm::uvec3(32,32,16);
      m_pageTableCacheNumBlocks = glm::uvec3(8, 8, 1); // 256*256*32*4*4   32MB
    } else if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() >= 1024) {
      m_imageCacheNumBlocks = glm::uvec3(32,32,8);
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 2); // 128*128*64*4*4   16MB
    } else {
      m_imageCacheNumBlocks = glm::uvec3(32,32,4);
      m_pageTableCacheNumBlocks = glm::uvec3(4, 4, 1); // 128*128*32*4*4   8MB
    }
#endif

    m_pageTableCacheSize = glm::ivec3(m_pageTableBlockSize * m_pageTableCacheNumBlocks);
    m_pageTableCacheTexture.reset(new Z3DTexture(GLint(GL_RGBA32I), glm::uvec3(m_pageTableCacheSize), GL_RGBA_INTEGER, GL_INT));
    m_pageTableCache.resize(m_pageTableCacheTexture->numPixels(), glm::ivec4(0,0,0,m_unmappedFlag));
    m_pageTableCacheTexture->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
    m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

    for (size_t c=0; c<m_imgPack.imgInfo().numChannels; ++c) {
      m_imageCacheTextures.emplace_back(new Z3DTexture(GLint(GL_R8), (m_imageBlockSize+2_u32) * m_imageCacheNumBlocks, GL_RED, GL_UNSIGNED_BYTE));
      m_imageCacheTextures[c]->uploadImage();
    }

    setScale(scale);
    //setScale(glm::vec3(1,1,5));
  }
}

Z3DImg::~Z3DImg()
{
}

QString Z3DImg::samplerType() const
{
  if (is3DData())
    return "sampler3D";
  else
    return "sampler2D";
}

std::vector<std::unique_ptr<Z3DVolume> > Z3DImg::makeXSliceVolume(size_t x)
{
  std::vector<std::unique_ptr<Z3DVolume>> res;
  size_t maxTextureSize = Z3DGpuInfoInstance.maxTextureSize();
  for (size_t c=0; c<m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(x,x+1,0,-1,0,-1,c,c+1,0,1));
    croped.infoRef().width = m_imgPack.imgInfo().height;
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>())
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    else
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    Z3DVolume *vh = new Z3DVolume(croped);
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
  size_t maxTextureSize = Z3DGpuInfoInstance.maxTextureSize();
  for (size_t c=0; c<m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0,-1,y,y+1,0,-1,c,c+1,0,1));
    croped.infoRef().height = m_imgPack.imgInfo().depth;
    croped.infoRef().depth = 1;
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>())
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    else
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    Z3DVolume *vh = new Z3DVolume(croped);
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
  size_t maxTextureSize = Z3DGpuInfoInstance.maxTextureSize();
  for (size_t c=0; c<m_nChannels; ++c) {
    ZImg croped = m_imgPack.crop(ZImgRegion(0,-1,0,-1,z,z+1,c,c+1,0,1));
    if (croped.width() > maxTextureSize || croped.height() > maxTextureSize) {
      croped = croped.resize(std::min(maxTextureSize, croped.width()),
                             std::min(maxTextureSize, croped.height()), 1);
    }
    if (!croped.isType<uint8_t>())
      croped = croped.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    else
      croped.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
    Z3DVolume *vh = new Z3DVolume(croped);
    vh->setVolColor(glm::vec3(m_imgPack.imgInfo().channelColors[c].r / 255.,
                              m_imgPack.imgInfo().channelColors[c].g / 255.,
                              m_imgPack.imgInfo().channelColors[c].b / 255.));
    res.emplace_back(vh);
  }
  return res;
}

std::vector<double> Z3DImg::physicalBoundBox() const
{
  glm::vec3 luf = physicalLUF();
  glm::vec3 rdb = physicalRDB();
  std::vector<double> res(6);
  res[0] = luf.x;
  res[1] = rdb.x;
  res[2] = luf.y;
  res[3] = rdb.y;
  res[4] = luf.z;
  res[5] = rdb.z;
  return res;
}

void Z3DImg::setScale(const glm::vec3 &scale)
{
  if (!m_isVolumeDownsampled) {
    return;
  }

  m_pageTableCacheManager.reset(new Z3DBlockCache<glm::ivec4>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, glm::ivec4(-1, -1, -1, -1)));
  m_imageCacheManager.reset(new Z3DBlockCache<glm::ivec4>(m_imageBlockSize+2_u32, m_imageCacheNumBlocks, glm::ivec4(-1, -1, -1, -1)));
  for (size_t c=0; c<m_channelPendingUpdates.size(); ++c) {
    m_channelPendingUpdates[c].clear();
  }

  const ZImgInfo& info = m_imgPack.imgInfo();
  glm::dvec3 imgDim = glm::dvec3(info.width, info.height, info.depth);
  glm::dvec3 relativeResolution = glm::dvec3(scale);
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
  assert(relativeResolution[sortedIndex[0]] == 1.0);
  for (size_t i=1; i<3; ++i) {
    double res = relativeResolution[sortedIndex[0]];
    while (true) {
      if (res*2 < relativeResolution[sortedIndex[i]]) {
        res *= 2;
        ++stayRounds[sortedIndex[i]];
      } else {
        if ((res*2 - relativeResolution[sortedIndex[i]]) < (relativeResolution[sortedIndex[i]] - res)) {
          ++stayRounds[sortedIndex[i]];
        }
        break;
      }
    }
  }

  m_pageDirectorySize = glm::ivec3(0, 0, 0);
  m_levelScales.resize(m_numLevels);
  m_imageDimensions.resize(m_numLevels);
  m_imageBounds.resize(m_numLevels);
  m_pageTableDimensions.resize(m_numLevels);
  m_pageDirectoryDimensions.resize(m_numLevels);
  m_posToBlockIDs.resize(m_numLevels);
  m_pageDirectoryBases.resize(m_numLevels);
  for (size_t l=0; l<m_numLevels; ++l) {
    if (l == 0) {
      m_levelScales[l] = glm::uvec3(1, 1, 1);
    } else {
      if (stayRounds[sortedIndex[2]] > stayRounds[sortedIndex[1]]) {
        --stayRounds[sortedIndex[2]];
        m_levelScales[l][sortedIndex[2]] = m_levelScales[l-1][sortedIndex[2]];
        m_levelScales[l][sortedIndex[1]] = m_levelScales[l-1][sortedIndex[1]] * 2;
        m_levelScales[l][sortedIndex[0]] = m_levelScales[l-1][sortedIndex[0]] * 2;
      } else if (stayRounds[sortedIndex[2]] > 0) {
        assert(stayRounds[sortedIndex[2]] == stayRounds[sortedIndex[1]]);
        --stayRounds[sortedIndex[2]];
        --stayRounds[sortedIndex[1]];
        m_levelScales[l][sortedIndex[2]] = m_levelScales[l-1][sortedIndex[2]];
        m_levelScales[l][sortedIndex[1]] = m_levelScales[l-1][sortedIndex[1]];
        m_levelScales[l][sortedIndex[0]] = m_levelScales[l-1][sortedIndex[0]] * 2;
      } else {
        m_levelScales[l] = m_levelScales[l-1] * 2_u32;
      }
    }
    assert(m_levelScales[l].x == m_levelScales[l].y);

    m_imageDimensions[l] = glm::uvec3((info.width + m_levelScales[l].x - 1) / m_levelScales[l].x,
                                      (info.height + m_levelScales[l].y - 1) / m_levelScales[l].y,
                                      (info.depth + m_levelScales[l].z - 1) / m_levelScales[l].z);
    m_imageBounds[l] = m_imageDimensions[l]-1_u32;
    m_pageTableDimensions[l] = glm::uvec3(m_imageDimensions[l] + m_imageBlockSize - 1_u32) / m_imageBlockSize;
    m_pageDirectoryDimensions[l] = glm::uvec3(m_pageTableDimensions[l] + m_pageTableBlockSize - 1_u32) / m_pageTableBlockSize;

    // id starts from 1
    m_posToBlockIDs[l] = glm::uvec4(1,
                                    m_pageTableDimensions[l].x,
                                    m_pageTableDimensions[l].x * m_pageTableDimensions[l].y,
                                    l == 0 ? 1 : (m_posToBlockIDs[l-1].w + m_pageTableDimensions[l-1].x * m_pageTableDimensions[l-1].y * m_pageTableDimensions[l-1].z));
    if (l == 0) {
      m_pageDirectoryBases[l] = glm::ivec3(0, 0, 0);
    } else if (l == 1) {
      m_pageDirectoryBases[l] = m_pageDirectoryBases[l-1];
      m_pageDirectoryBases[l][sortedIndex[1]] += m_pageDirectoryDimensions[l-1][sortedIndex[1]];
    } else {
      m_pageDirectoryBases[l] = m_pageDirectoryBases[l-1];
      m_pageDirectoryBases[l][sortedIndex[0]] += m_pageDirectoryDimensions[l-1][sortedIndex[0]];
    }
    m_pageDirectorySize = glm::max(m_pageDirectorySize, m_pageDirectoryBases[l] + glm::ivec3(m_pageDirectoryDimensions[l]));
    if (m_pageDirectorySize.x > Z3DGpuInfoInstance.max3DTextureSize() ||
        m_pageDirectorySize.y > Z3DGpuInfoInstance.max3DTextureSize() ||
        m_pageDirectorySize.z > Z3DGpuInfoInstance.max3DTextureSize()) {
      throw ZGLException(QString("Image (%1) is not supported").arg(info.toQString()));
    }
    LINFO() << l << " "
            << m_pageDirectoryDimensions[l] << " "
            << m_pageTableDimensions[l] << " "
            << m_imageDimensions[l] << " "
            << m_levelScales[l] << " "
            << m_posToBlockIDs[l];
  }

  // content of RGBA32I texture
  m_pageDirectoryTexture.reset(new Z3DTexture(GL_TEXTURE_3D, GLint(GL_RGBA32I), glm::uvec3(m_pageDirectorySize), GL_RGBA_INTEGER, GL_INT));
  m_pageDirectory.resize(m_pageDirectoryTexture->numPixels());
  memset(m_pageDirectory.data(), 0, m_pageDirectory.size() * sizeof(glm::ivec4));
  m_pageDirectoryTexture->setFilter(GLint(GL_NEAREST), GLint(GL_NEAREST));
  m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());

  memset(m_pageTableCache.data(), 0, m_pageTableCache.size() * sizeof(glm::ivec4));
  m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

  m_voxelWorldDimensions.resize(m_numLevels);
  m_voxelWorldSizes.resize(m_numLevels);
  for (size_t l=0; l<m_numLevels; ++l) {
    m_voxelWorldDimensions[l] = scale * glm::vec3(m_levelScales[l]);
    m_voxelWorldSizes[l] = std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
  }
}

void Z3DImg::bindFullResBlockIDsShader(Z3DShaderProgram &shader) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", glm::ivec3(m_pageTableBlockSize));
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", glm::ivec3(m_imageBlockSize));
  shader.setUniformArray("pos_to_block_ids", m_posToBlockIDs.data(), m_numLevels);
}

void Z3DImg::bindFullResRenderShader(Z3DShaderProgram &shader) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", glm::ivec3(m_pageTableBlockSize));
  shader.setUniformArray("image_dimensions", m_imageBounds.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", glm::ivec3(m_imageBlockSize));
  shader.setUniform("image_address_to_normalized_texture_coord", 1.f / glm::vec3(m_imageCacheTextures[0]->dimension() * 2_u32));
}

void Z3DImg::bindImageCacheToFullResRenderShader(Z3DShaderProgram &shader, size_t c) const
{
  shader.bindTexture("image_cache", m_imageCacheTextures[c].get());
}

bool Z3DImg::updateAndUploadPageDirectoryCaches(const std::set<uint32_t> &missingBlockIDs, const std::set<uint32_t> &usedBlockIDs)
{
  int numBlocksToRead = int(m_imageCacheManager->size()) - int(usedBlockIDs.size());
  if (missingBlockIDs.empty() || numBlocksToRead <= 0)
    return false;

  ZBenchTimer bt("update page table");
  bt.start();

  std::set<glm::ivec4, Vec4Compare<int, glm::highp>> usedPageTableKeys;
  size_t level = 0;
  for (uint32_t blockID : usedBlockIDs) {  // blockID must be ordered, can not use unordered_set here
    while (level+1 < m_numLevels && blockID >= m_posToBlockIDs[level+1].w) {
      ++level;
    }

    blockID -= m_posToBlockIDs[level].w;
    int z = blockID / m_posToBlockIDs[level].z;
    blockID -= z * m_posToBlockIDs[level].z;
    int y = blockID / m_posToBlockIDs[level].y;
    blockID -= y * m_posToBlockIDs[level].y;
    glm::ivec4 blockKey(level, blockID, y, z);
    usedPageTableKeys.insert(blockKey / glm::ivec4(1, m_pageTableBlockSize));
    m_imageCacheManager->touch(blockKey);
  }
  for (const glm::ivec4& key : usedPageTableKeys) {
    m_pageTableCacheManager->touch(key);
  }

  int count = 0;
  level = 0;
  glm::ivec4 erasedKey;
  int numAvailablePageCacheBlock = int(m_pageTableCacheManager->size()) - int(usedPageTableKeys.size());
  assert(numAvailablePageCacheBlock >= 0);
  for (auto it = missingBlockIDs.begin(); it != missingBlockIDs.end() && count < numBlocksToRead; ++it) {
    uint32_t blockID = *it;
    while (level+1 < m_numLevels && blockID >= m_posToBlockIDs[level+1].w) {
      ++level;
    }

    glm::ivec4 pageTableEntryKey(level, blockID, 0, 0);
    pageTableEntryKey.y -= m_posToBlockIDs[level].w;
    pageTableEntryKey.w = pageTableEntryKey.y / m_posToBlockIDs[level].z;
    pageTableEntryKey.y -= pageTableEntryKey.w * m_posToBlockIDs[level].z;
    pageTableEntryKey.z = pageTableEntryKey.y / m_posToBlockIDs[level].y;
    pageTableEntryKey.y -= pageTableEntryKey.z * m_posToBlockIDs[level].y;
    if (!glm::all(glm::lessThan(pageTableEntryKey.yzw(), glm::ivec3(m_pageTableDimensions[level]))) ||
        !glm::all(glm::greaterThanEqual(pageTableEntryKey.yzw(), glm::ivec3(0)))) {
      LINFO() << pageTableEntryKey << " " << m_pageTableDimensions[level];
      assert(false);
    }
    glm::ivec4 pageDirectoryEntryKey = pageTableEntryKey / glm::ivec4(1, m_pageTableBlockSize);
    glm::ivec3 pageDirectoryEntryCoord = m_pageDirectoryBases[pageDirectoryEntryKey.x] + pageDirectoryEntryKey.yzw();
    glm::ivec4& pageDirectoryEntry = m_pageDirectory[pageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
        pageDirectoryEntryCoord.y * m_pageDirectorySize.x + pageDirectoryEntryCoord.x];
    glm::ivec3 pageTableEntryCoord;

    if (pageDirectoryEntry.w > 0) {
      pageTableEntryCoord = pageDirectoryEntry.xyz() + pageTableEntryKey.yzw() % glm::ivec3(m_pageTableBlockSize);
      if (m_pageTableCache[pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x].w != 0) {
        LERROR() << "missing block is already mapped! " << pageTableEntryKey << " " << pageDirectoryEntryKey;
        continue;
      }
    }

    glm::ivec3 imageBlockCachePos = m_imageCacheManager->insert(pageTableEntryKey, erasedKey);
    //LINFO() << blockKey << " " << erasedKey << " " << m_posToBlockIDs[level] << " " << blockID << " " << level;
    if (erasedKey.x >= 0) { //valid
      glm::ivec4 erasedKeyPageDirectoryEntryKey = erasedKey / glm::ivec4(1, glm::ivec3(m_pageTableBlockSize));
      glm::ivec3 erasedKeyPageDirectoryEntryCoord = m_pageDirectoryBases[erasedKeyPageDirectoryEntryKey.x] + erasedKeyPageDirectoryEntryKey.yzw();
      glm::ivec4& erasedKeyPageDirectoryEntry = m_pageDirectory[erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
          erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x + erasedKeyPageDirectoryEntryCoord.x];

      if (erasedKeyPageDirectoryEntry.w > 0) {
        glm::ivec3 erasedKeyPageTableEntryCoord = erasedKeyPageDirectoryEntry.xyz() + erasedKey.yzw() % glm::ivec3(m_pageTableBlockSize);
        glm::ivec4& erasedKeyPageTableEntry = m_pageTableCache[erasedKeyPageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
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
        glm::ivec3 pageTableBlockCachePos = m_pageTableCacheManager->insert(pageDirectoryEntryKey, erasedKey);
        pageDirectoryEntry = glm::ivec4(pageTableBlockCachePos, 1);

        if (erasedKey.x >= 0) {
          for (size_t z=0; z<m_pageTableBlockSize.z; ++z) {
            for (size_t y=0; y<m_pageTableBlockSize.y; ++y) {
              memset(&m_pageTableCache[(pageTableBlockCachePos.z+z) * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
                  (pageTableBlockCachePos.y+y) * m_pageTableCacheSize.x + pageTableBlockCachePos.x],
                  0,
                  m_pageTableBlockSize.x * sizeof(glm::ivec4));
            }
          }

          glm::ivec3 erasedKeyPageDirectoryEntryCoord = m_pageDirectoryBases[erasedKey.x] + erasedKey.yzw();
          glm::ivec4& erasedKeyPageDirectoryEntry = m_pageDirectory[erasedKeyPageDirectoryEntryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
              erasedKeyPageDirectoryEntryCoord.y * m_pageDirectorySize.x + erasedKeyPageDirectoryEntryCoord.x];
          erasedKeyPageDirectoryEntry.w = 0;
        }

        pageTableEntryCoord = pageDirectoryEntry.xyz() + pageTableEntryKey.yzw() % glm::ivec3(m_pageTableBlockSize);
        glm::ivec4& pageTableEntry = m_pageTableCache[pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
            pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x];
        pageTableEntry = glm::ivec4(imageBlockCachePos, 1);
        --numAvailablePageCacheBlock;
      } else {
        m_imageCacheManager->popFront();
        LERROR() << "no space for new page table block, skip current image block";
        continue;
      }
    } else { // page directory mapped
      assert(pageDirectoryEntry.w > 0);
      m_pageTableCache[pageTableEntryCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          pageTableEntryCoord.y * m_pageTableCacheSize.x + pageTableEntryCoord.x] = glm::ivec4(imageBlockCachePos, 1);
      ++pageDirectoryEntry.w;
    }

    glm::ivec4 blockImagePos = pageTableEntryKey * glm::ivec4(1, glm::ivec3(m_imageBlockSize));
    glm::uvec3 blockCachePos = glm::uvec3(imageBlockCachePos);
    for (size_t c=0; c<m_channelPendingUpdates.size(); ++c) {
      m_channelPendingUpdates[c][blockCachePos] = blockImagePos;
    }
    ++count;
  }
  m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());
  m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());
  //glFinish();
  bt.stopAndLog();

  //checkPageSystemError();

  return count > 0;
}

void Z3DImg::uploadImageCache(size_t channel)
{
  if (m_channelPendingUpdates[channel].empty())
    return;

  ZBenchTimer bt("upload image cache");
  bt.start();

  ZImg img(ZImgInfo(m_imageBlockSize.x+2, m_imageBlockSize.y+2, m_imageBlockSize.z+2, 1));
  if (m_imageBlockReadSize == glm::ivec3(m_imageBlockSize)) {
    for (auto it = m_channelPendingUpdates[channel].cbegin(); it != m_channelPendingUpdates[channel].cend(); ++it) {
      const glm::ivec4& blockImagePos = it->second;
      m_imgPack.readRegionToImg(m_levelScales[blockImagePos.x].x, m_levelScales[blockImagePos.x].z,
          blockImagePos.y-1, blockImagePos.z-1, blockImagePos.w-1, channel, 0, img);
      m_imageCacheTextures[channel]->uploadSubImage(it->first, m_imageBlockSize+2_u32, img.channelData(0));
      img.fill(0);
    }
  } else {
    ZImg bigImg(ZImgInfo(m_imageBlockReadSize.x+2, m_imageBlockReadSize.y+2, m_imageBlockReadSize.z+2, 1));
    std::map<glm::ivec4, std::vector<std::pair<glm::ivec4, glm::uvec3>>, Vec4Compare<int,glm::highp>> bigToSmall;
    glm::ivec4 tmp(1, m_imageBlockReadSize.x, m_imageBlockReadSize.y, m_imageBlockReadSize.z);
    for (auto it = m_channelPendingUpdates[channel].cbegin(); it != m_channelPendingUpdates[channel].cend(); ++it) {
      bigToSmall[it->second / tmp * tmp].push_back(std::make_pair(it->second, it->first));
    }
    for (auto it = bigToSmall.begin(); it != bigToSmall.end(); ++it) {
      // read from it->first level x y z
      m_imgPack.readRegionToImg(m_levelScales[it->first.x].x, m_levelScales[it->first.x].z,
          it->first.y-1, it->first.z-1, it->first.w-1, channel, 0, bigImg);
      for (size_t i=0; i<it->second.size(); ++i) {
        glm::ivec3 startCoord = it->first.yzw() - it->second[i].first.yzw();
        img.pasteImg(bigImg, ZVoxelCoordinate(startCoord.x, startCoord.y, startCoord.z));
        m_imageCacheTextures[channel]->uploadSubImage(it->second[i].second, m_imageBlockSize+2_u32, img.channelData(0));
      }
      bigImg.fill(0);
    }
  }
  m_channelPendingUpdates[channel].clear();
  //glFinish();
  bt.stopAndLog();
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
  size_t maxPossibleChannels = Z3DGpuInfoInstance.maxArrayTextureLayers();
#endif
  if (m_nChannels > maxPossibleChannels) {
    QMessageBox::warning(QApplication::activeWindow(), "Too many channels",
                         QString("Due to hardware limit, only first %1 channels of this image will be shown").arg(maxPossibleChannels));
    m_nChannels = maxPossibleChannels;
  }

  double widthScale = 1.0;
  double heightScale = 1.0;
  double depthScale = 1.0;
  Z3DGpuInfoInstance.getDataScaleForTexture(info.width, info.height, info.depth, widthScale, heightScale, depthScale);

  if (widthScale != 1.0 || heightScale != 1.0 || depthScale != 1.0) {
    m_isVolumeDownsampled = true;

    if (m_imgPack.imgInfo().depth > 1) {
      widthScale = info.width <= 512_usize ? 1.0 : 512.0 / info.width;
      heightScale = info.height <= 512_usize ? 1.0 : 512.0 / info.height;
      depthScale = info.depth <= 512_usize ? 1.0 : 512.0 / info.depth;
    }

    //return;
  }

  ZImg img = m_imgPack.resizedImg(info.width*widthScale,
                                  info.height*heightScale,
                                  info.depth*depthScale,
                                  0);
  if (!img.isType<uint8_t>()) {
    img = img.convertTo<uint8_t>(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
  } else if (img.validBitCount() != 0 && img.validBitCount() != 8 && img.validBitCount() != 16) {
    img.normalize(m_imgPack.minIntensity(), m_imgPack.maxIntensity());
  }
  if (m_nChannels == 1) {
    Z3DVolume *vh = new Z3DVolume(img,
                                  glm::vec3(1.f/widthScale, 1.f/heightScale, 1.f/depthScale),
                                  glm::vec3(.0));

    m_volumes.emplace_back(vh);
  } else {
    for (size_t i=0; i<m_nChannels; i++) {
      ZImg cImg = img.crop(ZImgRegion(0,-1,0,-1,0,-1,i,i+1));
      Z3DVolume *vh = new Z3DVolume(cImg,
                                    glm::vec3(1.f/widthScale, 1.f/heightScale, 1.f/depthScale),
                                    glm::vec3(.0));

      m_volumes.emplace_back(vh);
    } //for each cannel
  }

  for (size_t i=0; i<m_nChannels; i++) {
    m_volumes[i]->setVolColor(glm::vec3(info.channelColors[i].r / 255.,
                                        info.channelColors[i].g / 255.,
                                        info.channelColors[i].b / 255.));
  }
}

void Z3DImg::checkPageSystemError()
{
  for (size_t i=0; i<m_pageDirectory.size(); ++i) {
    if (m_pageDirectory[i].w == 0) {
      continue;
    }
    assert(m_pageDirectory[i].w > 0);

    glm::ivec3 pdLoc;
    pdLoc.x = i;
    pdLoc.z = pdLoc.x / m_pageDirectorySize.x / m_pageDirectorySize.y;
    pdLoc.x -= pdLoc.z * m_pageDirectorySize.x * m_pageDirectorySize.y;
    pdLoc.y = pdLoc.x / m_pageDirectorySize.x;
    pdLoc.x -= pdLoc.y * m_pageDirectorySize.x;

    size_t level = 100000;

    for (size_t l=0; l<m_numLevels; ++l) {
      if (glm::all(glm::greaterThanEqual(pdLoc, m_pageDirectoryBases[l])) &&
          glm::all(glm::lessThan(pdLoc, m_pageDirectoryBases[l] + glm::ivec3(m_pageDirectoryDimensions[l])))) {
        level = l;
        pdLoc -= m_pageDirectoryBases[l];
        break;
      }
    }

    assert(level < 10000);

    glm::ivec4 pageTableKey(level, pdLoc);
    assert(m_pageTableCacheManager->exists(pageTableKey));
    assert(m_pageTableCacheManager->get(pageTableKey) == m_pageDirectory[i].xyz());
    assert(glm::all(glm::greaterThanEqual(m_pageDirectory[i].xyz(), glm::ivec3(0,0,0))) &&
           glm::all(glm::lessThan(m_pageDirectory[i].xyz(), m_pageTableCacheSize)));

    int numValidEntry = 0;
    for (size_t z=0; z<m_pageTableBlockSize.z; ++z) {
      for (size_t y=0; y<m_pageTableBlockSize.y; ++y) {
        for (size_t x=0; x<m_pageTableBlockSize.x; ++x) {
          glm::ivec4 pageTableEntry = m_pageTableCache[(m_pageDirectory[i].z + z) * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
              (m_pageDirectory[i].y + y) * m_pageTableCacheSize.x + m_pageDirectory[i].x + x];
          if (pageTableEntry.w > 0) {
            ++numValidEntry;
            glm::ivec4 imageCacheKey(level, glm::ivec3(x,y,z) + pdLoc * glm::ivec3(m_pageTableBlockSize));
            assert(m_imageCacheManager->exists(imageCacheKey));
            assert(m_imageCacheManager->get(imageCacheKey) == pageTableEntry.xyz());
            assert(glm::all(glm::greaterThanEqual(pageTableEntry.xyz(), glm::ivec3(0,0,0))) &&
                   glm::all(glm::lessThan(pageTableEntry.xyz(), glm::ivec3(m_imageCacheTextures[0]->dimension()))));
          }
        }
      }
    }
    assert(numValidEntry == m_pageDirectory[i].w);
  }
}

} // namespace nim


