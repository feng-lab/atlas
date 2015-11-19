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

Z3DImg::Z3DImg(ZImgPack &imgPack, QObject *parent)
  : QObject(parent)
  , m_imgPack(imgPack)
{
  const ZImgInfo& info = m_imgPack.imgInfo();
  glm::dvec3 imgDim = glm::dvec3(info.width, info.height, info.depth);
  glm::dvec3 relativeResolution = glm::dvec3(info.voxelSizeXInUm(), info.voxelSizeYInUm(), info.voxelSizeZInUm());
  double minRes = std::min(std::min(relativeResolution.x, relativeResolution.y), relativeResolution.z);
  relativeResolution /= minRes;
  imgDim *= relativeResolution;
  double maxImgDim = std::max(std::max(imgDim.x, imgDim.y), imgDim.z);
  m_numLevels = std::ceil(std::log2(maxImgDim / m_imageBlockSize)) + 1;

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
        m_levelScales[l] = m_levelScales[l-1] * 2;
      }
    }
    m_imageDimensions.push_back(glm::uvec3((info.width + m_levelScales[l].x - 1) / m_levelScales[l].x,
                                           (info.height + m_levelScales[l].y - 1) / m_levelScales[l].y,
                                           (info.depth + m_levelScales[l].z - 1) / m_levelScales[l].z));
    m_pageTableDimensions.push_back(glm::uvec3((m_imageDimensions[l].x + m_imageBlockSize - 1) / m_imageBlockSize,
                                               (m_imageDimensions[l].y + m_imageBlockSize - 1) / m_imageBlockSize,
                                               (m_imageDimensions[l].z + m_imageBlockSize - 1) / m_imageBlockSize));
    m_pageDirectoryDimensions.push_back(glm::uvec3((m_pageTableDimensions[l].x + m_pageTableBlockSize - 1) / m_pageTableBlockSize,
                                                   (m_pageTableDimensions[l].y + m_pageTableBlockSize - 1) / m_pageTableBlockSize,
                                                   (m_pageTableDimensions[l].z + m_pageTableBlockSize - 1) / m_pageTableBlockSize));

      // id starts from 1
    m_posToBlockIDs[l].push_back(glm::uvec4(1,
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
  m_pageDirectory.resize(size_t(pageDirectoryEnd.x) * pageDirectoryEnd.y * pageDirectoryEnd.z, glm::ivec4(0,0,0,0));


  //std::vector<glm::vec3> m_voxelWorldDimensions;
  //std::vector<float> m_voxelWorldSizes;
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

} // namespace nim


