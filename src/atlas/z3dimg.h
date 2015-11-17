#ifndef Z3DIMG_H
#define Z3DIMG_H

#include "z3dgl.h"
#include <QObject>
#include "zimgpack.h"
#include <QThread>

namespace nim {

class Z3DShaderProgram;
class Z3DTexture;

// Z3DVolume coordinates:
// 1. Voxel Coordinate:    [0, dim.x-1] x [0, dim.y-1] x [0, dim.z-1]
//                     in which (0,0,0) is LeftUpFront Corner (LUF)
//                         and dim-1 is RightDownBack Corner (RDB)
// 2. Texture Coordinate:  [0.0, 1.0] x [0.0, 1.0] x [0.0, 1.0]

class Z3DImg : public QObject
{
  Q_OBJECT
public:
  // Z3DVolume will take ownership of the img
  Z3DImg(ZImgPack &imgPack,
         QObject *parent = 0);
  virtual ~Z3DImg();

  bool is1DData() const { return m_voxelDimensions[0].z == 1 && (m_voxelDimensions[0].x == 1 || m_voxelDimensions[0].y == 1); }
  bool is2DData() const { return m_voxelDimensions[0].z == 1 && m_voxelDimensions[0].x > 1 && m_voxelDimensions[0].y > 1; }
  bool is3DData() const { return m_voxelDimensions[0].z > 1 && m_voxelDimensions[0].x > 1 && m_voxelDimensions[0].y > 1; }

  // Returns a string representation of the sampler type: "sampler2D" for 2D image, "sampler3D" for 3D volume
  QString samplerType() const;

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  glm::vec3 physicalLUF() const { return glm::vec3(0, 0, 0); }
  glm::vec3 physicalRDB() const { return glm::vec3(m_voxelDimensions[0]); }
  glm::vec3 physicalLDF() const { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalLUF().z); }
  glm::vec3 physicalRDF() const { return glm::vec3(physicalRDB().x, physicalRDB().y, physicalLUF().z); }
  glm::vec3 physicalRUF() const { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalLUF().z); }
  glm::vec3 physicalLUB() const { return glm::vec3(physicalLUF().x, physicalLUF().y, physicalRDB().z); }
  glm::vec3 physicalLDB() const { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalRDB().z); }
  glm::vec3 physicalRUB() const { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalRDB().z); }

  // xmin, xmax, ymin, ymax, zmin, zmax
  std::vector<double> physicalBoundBox() const;

signals:

protected slots:

protected:

protected:
  const ZImgPack& m_imgPack;

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

private:
  //std::unique_ptr<Z3DImgHistogramThread> m_histogramThread;
};

} // namespace nim

#endif // Z3DIMG_H
