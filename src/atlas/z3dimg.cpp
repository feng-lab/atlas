#include "z3dimg.h"
#include <algorithm>
#include <QString>
#include "QsLog.h"
#include "z3dshaderprogram.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"

namespace nim {

Z3DImg::Z3DImg(ZImgPack &imgPack, QObject *parent)
  : QObject(parent)
  , m_imgPack(imgPack)
{
  ZImg m_pageDirectory;
  ZImg m_pageTableCache;
  ZImg m_voxelCache;
  size_t m_pageTableBlockSize = 32;
  size_t m_voxelBlockSize = 32;
  size_t m_numLevels;
  std::vector<glm::ivec3> m_pageDirectoryBases;
  std::vector<glm::ivec3> m_pageDirectoryDimensions;
  std::vector<glm::ivec3> m_pageTableDimensions;
  std::vector<glm::ivec3> m_voxelDimensions;
  std::vector<float> m_voxelSizes;
  std::vector<glm::uvec4> m_posToBlockIDs;
}

Z3DImg::~Z3DImg()
{
}

QString Z3DImg::samplerType() const
{
  if (m_voxelDimensions[0].z > 1)
    return "sampler3D";
  else if (m_voxelDimensions[0].y > 1 && m_voxelDimensions[0].x > 1)
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


