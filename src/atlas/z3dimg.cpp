#include "z3dimg.h"
#include <algorithm>
#include <QString>
#include "QsLog.h"
#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zkmeans.h"
#include "zexception.h"

namespace nim {

Z3DImg::Z3DImg(ZImgPack &imgPack, const glm::vec3 &scale, QObject *parent)
  : QObject(parent)
  , m_imgPack(imgPack)
  , m_pageTableBlockSize(32, 32, 32)
  , m_pageTableCacheNumBlocks(8, 8, 2) // 256*256*64*4*4   64MB
  , m_imageBlockSize(32, 32, 32)
  , m_imageCacheNumBlocks(Z3DGpuInfoInstance.dedicatedVideoMemoryMB() > 1500 ? glm::uvec3(32,32,32) : glm::uvec3(32,32,16))
  , m_pageTableCacheManager(m_pageTableBlockSize, m_pageTableCacheNumBlocks, glm::ivec4(-1, -1, -1, -1))
  , m_imageCacheManager(m_imageBlockSize, m_imageCacheNumBlocks, glm::ivec4(-1, -1, -1, -1))
{
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
      } else if ((res*2 - relativeResolution[sortedIndex[i]]) < (relativeResolution[sortedIndex[i]] - res)) {
        ++stayRounds[sortedIndex[i]];
        break;
      }
    }
  }

  glm::ivec3 pageDirectoryEnd(0, 0, 0);
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
                                         l == 0 ? 1 : (1 + m_pageTableDimensions[l-1].x * m_pageTableDimensions[l-1].y * m_pageTableDimensions[l-1].z)));
    if (l == 0) {
      m_pageDirectoryBases.push_back(glm::ivec3(0, 0, 0));
    } else if (l == 1) {
      m_pageDirectoryBases.push_back(m_pageDirectoryBases[l-1]);
      m_pageDirectoryBases[l][sortedIndex[1]] += m_pageTableDimensions[l-1][sortedIndex[1]];
    } else {
      m_pageDirectoryBases.push_back(m_pageDirectoryBases[l-1]);
      m_pageDirectoryBases[l][sortedIndex[0]] += m_pageTableDimensions[l-1][sortedIndex[0]];
    }
    pageDirectoryEnd = glm::max(pageDirectoryEnd, m_pageDirectoryBases[l] + glm::ivec3(m_pageDirectoryDimensions[l]));
    if (pageDirectoryEnd.x > Z3DGpuInfoInstance.max3DTextureSize() ||
        pageDirectoryEnd.y > Z3DGpuInfoInstance.max3DTextureSize() ||
        pageDirectoryEnd.z > Z3DGpuInfoInstance.max3DTextureSize()) {
      throw ZGLException(QString("Image (%1) is not supported").arg(info.toQString()));
    }
  }

  // content of RGBA32I texture
  m_pageDirectoryTexture.reset(new Z3DTexture(pageDirectoryEnd, GL_RGBA_INTEGER, (GLint)GL_RGBA32I, GL_INT));
  m_pageDirectory.resize(m_pageDirectoryTexture->numPixels(), glm::ivec4(0,0,0,m_unmappedFlag));
  m_pageDirectoryTexture->setData(m_pageDirectory.data());
  m_pageDirectoryTexture->uploadTexture();

  m_pageTableCacheTexture.reset(new Z3DTexture(glm::ivec3(m_pageTableBlockSize * m_pageTableCacheNumBlocks), GL_RGBA_INTEGER, (GLint)GL_RGBA32I, GL_INT));
  m_pageTableCache.resize(m_pageTableCacheTexture->numPixels(), glm::ivec4(0,0,0,m_unmappedFlag));
  m_pageTableCacheTexture->setData(m_pageTableCache.data());
  m_pageTableCacheTexture->uploadTexture();

  for (size_t c=0; c<info.numChannels; ++c) {
    m_imageCacheTextures.emplace_back(new Z3DTexture(glm::ivec3(m_imageBlockSize * m_imageCacheNumBlocks), GL_RED, (GLint)GL_R8, GL_UNSIGNED_BYTE));
    m_imageCacheTextures[c]->uploadTexture();
  }

  setScale(scale);
}

Z3DImg::~Z3DImg()
{
}

QString Z3DImg::samplerType() const
{
  if (m_imageDimensions[0].z > 1)
    return "sampler3D";
  else if (m_imageDimensions[0].y > 1 && m_imageDimensions[0].x > 1)
    return "sampler2D";
  else
    return "sampler1D";
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
    m_voxelWorldDimensions[l] = scale * glm::vec3(m_levelScales);
    m_voxelWorldSizes[l] = std::min(std::min(m_voxelWorldDimensions[l].x, m_voxelWorldDimensions[l].y), m_voxelWorldDimensions[l].z);
  }
}

} // namespace nim


