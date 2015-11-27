#include "z3dimg.h"
#include <algorithm>
#include <QString>
#include "QsLog.h"
#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zkmeans.h"
#include "zexception.h"
#include <QApplication>
#include <QMessageBox>

namespace nim {

Z3DImg::Z3DImg(const ZImgPack &imgPack, const glm::vec3 &scale, QObject *parent)
  : QObject(parent)
  , m_imgPack(imgPack)
  , m_isVolumeDownsampled(false)
{
  // directX 10 resource limit
  // 128 MB
  // directX 11 resource limit
  //min(max(128, 0.25f * (amount of dedicated VRAM)), 2048) MB
  //D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM (128)
  //D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_B_TERM (0.25f)
  //D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM (2048)
  size_t currentAvailableTexMem = Z3DGpuInfoInstance.dedicatedVideoMemoryMB();
  m_maxVoxelNumber = std::min(std::max(size_t(128), static_cast<size_t>(0.25 * currentAvailableTexMem)), size_t(2048)) * 1024 * 1024;
  readVolumes();

  if (m_isVolumeDownsampled) {
    m_pageTableBlockSize = glm::uvec3(32, 32, 32);
    m_pageTableCacheNumBlocks = glm::uvec3(8, 8, 2); // 256*256*64*4*4   64MB
    m_imageBlockSize = glm::uvec3(32, 32, 32);
    if (Z3DGpuInfoInstance.dedicatedVideoMemoryMB() > 1500) {
      m_imageCacheNumBlocks = glm::uvec3(32,32,32);
    } else {
      m_imageCacheNumBlocks = glm::uvec3(32,32,16);
    }
    m_pageTableCacheManager.reset(new Z3DBlockCache<glm::ivec4>(m_pageTableBlockSize, m_pageTableCacheNumBlocks, glm::ivec4(-1, -1, -1, -1)));
    m_imageCacheManager.reset(new Z3DBlockCache<glm::ivec4>(m_imageBlockSize, m_imageCacheNumBlocks, glm::ivec4(-1, -1, -1, -1)));

    const ZImgInfo& info = m_imgPack.imgInfo();
    glm::dvec3 imgDim = glm::dvec3(info.width, info.height, info.depth);
    glm::dvec3 relativeResolution = glm::dvec3(info.voxelSizeXInUm(), info.voxelSizeYInUm(), info.voxelSizeZInUm());
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

    LINFO() << m_numLevels;

    m_pageDirectorySize = glm::ivec3(0, 0, 0);
    for (size_t l=0; l<m_numLevels; ++l) {
      m_levelScales.push_back(glm::uvec3(1, 1, 1));
      if (l > 0) {
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
          m_levelScales[l] = m_levelScales[l-1] * uint32_t(2);
        }
      }
      m_imageDimensions.push_back(glm::uvec3((info.width + m_levelScales[l].x - 1) / m_levelScales[l].x,
                                             (info.height + m_levelScales[l].y - 1) / m_levelScales[l].y,
                                             (info.depth + m_levelScales[l].z - 1) / m_levelScales[l].z));
      m_pageTableDimensions.push_back(glm::uvec3(m_imageDimensions[l] + m_imageBlockSize - uint32_t(1)) / m_imageBlockSize);
      m_pageDirectoryDimensions.push_back(glm::uvec3(m_pageTableDimensions[l] + m_pageTableBlockSize - uint32_t(1)) / m_pageTableBlockSize);

      // id starts from 1
      m_posToBlockIDs.push_back(glm::uvec4(1,
                                           m_pageTableDimensions[l].x,
                                           m_pageTableDimensions[l].x * m_pageTableDimensions[l].y,
                                           l == 0 ? 1 : (m_posToBlockIDs[l-1].w + m_pageTableDimensions[l-1].x * m_pageTableDimensions[l-1].y * m_pageTableDimensions[l-1].z)));
      if (l == 0) {
        m_pageDirectoryBases.push_back(glm::ivec3(0, 0, 0));
      } else if (l == 1) {
        m_pageDirectoryBases.push_back(m_pageDirectoryBases[l-1]);
        m_pageDirectoryBases[l][sortedIndex[1]] += m_pageDirectoryDimensions[l-1][sortedIndex[1]];
      } else {
        m_pageDirectoryBases.push_back(m_pageDirectoryBases[l-1]);
        m_pageDirectoryBases[l][sortedIndex[0]] += m_pageDirectoryDimensions[l-1][sortedIndex[0]];
      }
      m_pageDirectorySize = glm::max(m_pageDirectorySize, m_pageDirectoryBases[l] + glm::ivec3(m_pageDirectoryDimensions[l]));
      if (m_pageDirectorySize.x > Z3DGpuInfoInstance.max3DTextureSize() ||
          m_pageDirectorySize.y > Z3DGpuInfoInstance.max3DTextureSize() ||
          m_pageDirectorySize.z > Z3DGpuInfoInstance.max3DTextureSize()) {
        throw ZGLException(QString("Image (%1) is not supported").arg(info.toQString()));
      }
      LINFO() << m_pageDirectoryBases[l] << m_pageDirectoryDimensions[l] << m_posToBlockIDs[l];
    }

    // content of RGBA32I texture
    m_pageDirectoryTexture.reset(new Z3DTexture(GL_TEXTURE_3D, (GLint)GL_RGBA32I, glm::uvec3(m_pageDirectorySize), GL_RGBA_INTEGER, GL_INT));
    m_pageDirectory.resize(m_pageDirectoryTexture->numPixels(), glm::ivec4(0,0,0,m_unmappedFlag));
    m_pageDirectoryTexture->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
    m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());

    m_pageTableCacheSize = glm::ivec3(m_pageTableBlockSize * m_pageTableCacheNumBlocks);
    m_pageTableCacheTexture.reset(new Z3DTexture(GL_TEXTURE_3D, (GLint)GL_RGBA32I, glm::uvec3(m_pageTableCacheSize), GL_RGBA_INTEGER, GL_INT));
    m_pageTableCache.resize(m_pageTableCacheTexture->numPixels(), glm::ivec4(0,0,0,m_unmappedFlag));
    m_pageTableCacheTexture->setFilter((GLint)GL_NEAREST, (GLint)GL_NEAREST);
    m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

    for (size_t c=0; c<info.numChannels; ++c) {
      m_imageCacheTextures.emplace_back(new Z3DTexture((GLint)GL_R8, m_imageBlockSize * m_imageCacheNumBlocks, GL_RED, GL_UNSIGNED_BYTE));
      m_imageCacheTextures[c]->uploadImage();
    }

    setScale(scale);
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
      croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
    else
      croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
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
      croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
    else
      croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
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
      croped = croped.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
    else
      croped.normalize(m_imgMinIntensity, m_imgMaxIntensity);
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
  m_voxelWorldDimensions.resize(m_numLevels);
  m_voxelWorldSizes.resize(m_numLevels);
  for (size_t l=0; l<m_numLevels; ++l) {
    m_voxelWorldDimensions[l] = scale * glm::vec3(m_levelScales[l]);
    m_voxelWorldSizes[l] = std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
  }
}

void Z3DImg::bindFullResBlockIDsShader(Z3DShaderProgram &shader, size_t c) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", glm::ivec3(m_pageTableBlockSize));
  shader.setUniformArray("image_dimensions", m_imageDimensions.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", glm::ivec3(m_imageBlockSize));
  shader.setUniformArray("pos_to_block_ids", m_posToBlockIDs.data(), m_numLevels);
}

void Z3DImg::bindFullResRenderShader(Z3DShaderProgram &shader, size_t c) const
{
  shader.bindTexture("page_directory", m_pageDirectoryTexture.get());
  shader.setUniformArray("page_directory_bases", m_pageDirectoryBases.data(), m_numLevels);
  shader.bindTexture("page_table_cache", m_pageTableCacheTexture.get());
  shader.setUniform("page_table_block_size", glm::ivec3(m_pageTableBlockSize));
  shader.bindTexture("image_cache", m_imageCacheTextures[c].get());
  shader.setUniformArray("image_dimensions", m_imageDimensions.data(), m_numLevels);
  shader.setUniformArray("voxel_world_sizes", m_voxelWorldSizes.data(), m_numLevels);
  shader.setUniform("image_block_size", glm::ivec3(m_imageBlockSize));
}

bool Z3DImg::updateCaches(const std::set<uint32_t> &missingBlockIDs, const std::set<uint32_t> &usedBlockIDs)
{
  int numBlocksToRead = int(m_imageCacheManager->size()) - int(usedBlockIDs.size());
  if (missingBlockIDs.empty() || numBlocksToRead <= 0)
    return false;

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
  std::vector<std::pair<glm::ivec4, glm::ivec3>> blocksToRead;
  for (auto it = missingBlockIDs.begin(); it != missingBlockIDs.end() && count < numBlocksToRead; ++it) {
    uint32_t blockID = *it;
    while (level+1 < m_numLevels && blockID >= m_posToBlockIDs[level+1].w) {
      ++level;
    }

    blockID -= m_posToBlockIDs[level].w;
    int z = blockID / m_posToBlockIDs[level].z;
    blockID -= z * m_posToBlockIDs[level].z;
    int y = blockID / m_posToBlockIDs[level].y;
    blockID -= y * m_posToBlockIDs[level].y;
    glm::ivec4 blockKey(level, blockID, y, z);
    glm::ivec4 pageTableKey = blockKey / glm::ivec4(1, m_pageTableBlockSize);

    glm::ivec3 blockPos = m_imageCacheManager->insert(blockKey, erasedKey);
    //LINFO() << blockKey << erasedKey << m_posToBlockIDs[level] << blockID << level;
    if (erasedKey.x >= 0) { //valid
      glm::ivec4 erasedKeyPageTableKey = erasedKey / glm::ivec4(1, glm::ivec3(m_pageTableBlockSize));
      glm::ivec3 pageDirectoryCoord = m_pageDirectoryBases[erasedKeyPageTableKey.x] + erasedKeyPageTableKey.yzw();
      glm::ivec4& pageDirectoryContent = m_pageDirectory[pageDirectoryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
          pageDirectoryCoord.y * m_pageDirectorySize.x + pageDirectoryCoord.x];
      --pageDirectoryContent.w;
      assert(pageDirectoryContent.w >= 0);

      glm::ivec3 pageTableCacheCoord = pageDirectoryContent.xyz() + erasedKey.yzw() % glm::ivec3(m_pageTableBlockSize);
      glm::ivec4& pageTableCacheContent = m_pageTableCache[pageTableCacheCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          pageTableCacheCoord.y * m_pageTableCacheSize.x + pageTableCacheCoord.x];
      pageTableCacheContent.w = 0;
      if (pageDirectoryContent.w == 0) {
        // unmap entire page table block
        m_pageTableCacheManager->remove(erasedKeyPageTableKey);
        ++numAvailablePageCacheBlock;
      }
    }

    glm::ivec3 pageDirectoryCoord = m_pageDirectoryBases[pageTableKey.x] + pageTableKey.yzw();
    glm::ivec4& pageDirectoryContent = m_pageDirectory[pageDirectoryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
        pageDirectoryCoord.y * m_pageDirectorySize.x + pageDirectoryCoord.x];
    if (pageDirectoryContent.w == 0) { // page directory unmapped
      if (numAvailablePageCacheBlock > 0) { // construct new page table block
        glm::ivec3 pageTableBlockPos = m_pageTableCacheManager->insert(pageTableKey, erasedKey);
        pageDirectoryContent = glm::ivec4(pageTableBlockPos, 1);

        if (erasedKey.x >= 0) {
          glm::ivec3 erasedKeyPageDirectoryCoord = m_pageDirectoryBases[erasedKey.x] + erasedKey.yzw();
          glm::ivec4& erasedKeyPageDirectoryContent = m_pageDirectory[erasedKeyPageDirectoryCoord.z * m_pageDirectorySize.x * m_pageDirectorySize.y +
              erasedKeyPageDirectoryCoord.y * m_pageDirectorySize.x + erasedKeyPageDirectoryCoord.x];
          erasedKeyPageDirectoryContent.w = 0;
        }

        glm::ivec3 pageTableCacheCoord = pageDirectoryContent.xyz() + blockKey.yzw() % glm::ivec3(m_pageTableBlockSize);
        glm::ivec4& pageTableCacheContent = m_pageTableCache[pageTableCacheCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
            pageTableCacheCoord.y * m_pageTableCacheSize.x + pageTableCacheCoord.x];
        pageTableCacheContent = glm::ivec4(blockPos, 1);
        --numAvailablePageCacheBlock;
      } else { // no space for new page table block, skip current image block
        m_imageCacheManager->popFront();
        continue;
      }
    } else { // page directory mapped
      assert(pageDirectoryContent.w > 0);
      glm::ivec3 pageTableCacheCoord = pageDirectoryContent.xyz() + blockKey.yzw() % glm::ivec3(m_pageTableBlockSize);
      glm::ivec4& pageTableCacheContent = m_pageTableCache[pageTableCacheCoord.z * m_pageTableCacheSize.x * m_pageTableCacheSize.y +
          pageTableCacheCoord.y * m_pageTableCacheSize.x + pageTableCacheCoord.x];
      pageTableCacheContent = glm::ivec4(blockPos, 1);
      ++pageDirectoryContent.w;
    }

    blocksToRead.push_back(std::make_pair(blockKey * glm::ivec4(1, glm::ivec3(m_imageBlockSize)), blockPos));
    ++count;
  }

  for (size_t i=0; i<blocksToRead.size(); ++i) {
    const glm::ivec4& blockImagePos = blocksToRead[i].first;  // level, x, y, z
    const glm::ivec3& blockCachePos = blocksToRead[i].second;
    // actual read and upload

  }

  m_pageDirectoryTexture->uploadImage(m_pageDirectory.data());
  m_pageTableCacheTexture->uploadImage(m_pageTableCache.data());

  return count > 0;
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

  bool scaleZ = info.depth > std::pow(m_maxVoxelNumber, 1/3.0);
  double scale = 1.0;
  if (info.timeVoxelNumber() > m_maxVoxelNumber) {
    if (scaleZ)
      scale = std::pow((m_maxVoxelNumber*1.0) / info.timeVoxelNumber(), 1/3.0);
    else
      scale = std::sqrt((m_maxVoxelNumber*1.0) / info.timeVoxelNumber());
  }
  int height = static_cast<int>(info.height * scale);
  int width = static_cast<int>(info.width * scale);
  int depth = scaleZ ? static_cast<int>(info.depth * scale) : static_cast<int>(info.depth);
  double widthScale = 1.0;
  double heightScale = 1.0;
  double depthScale = 1.0;
  int maxTextureSize = 100;
  if (info.depth > 1)
    maxTextureSize = Z3DGpuInfoInstance.max3DTextureSize();
  else
    maxTextureSize = Z3DGpuInfoInstance.maxTextureSize();

  if (height > maxTextureSize) {
    heightScale = static_cast<double>(maxTextureSize) / height;
    height = std::floor(height * heightScale);
  }
  if (width > maxTextureSize) {
    widthScale = static_cast<double>(maxTextureSize) / width;
    width = std::floor(width * widthScale);
  }
  if (depth > maxTextureSize) {
    depthScale = static_cast<double>(maxTextureSize) / depth;
    depth = std::floor(depth * depthScale);
  }

  widthScale *= scale;
  heightScale *= scale;
  if (scaleZ)
    depthScale *= scale;

  if (widthScale != 1.0 || heightScale != 1.0 || depthScale != 1.0) {
    m_isVolumeDownsampled = true;
    return;
  }

  ZImg img = m_imgPack.resizedImg(width, height, depth, 0);
  img.computeMinMax(m_imgMinIntensity, m_imgMaxIntensity);
  if (!img.isType<uint8_t>()) {
    img = img.convertTo<uint8_t>(m_imgMinIntensity, m_imgMaxIntensity);
  } else/* if (img.validBitCount() != 0 && img.validBitCount() < 8) */{
    img.normalize(m_imgMinIntensity, m_imgMaxIntensity);
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

} // namespace nim


