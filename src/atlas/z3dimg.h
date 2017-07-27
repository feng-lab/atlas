#pragma once

#include "z3dgl.h"
#include "zimgpack.h"
#include "z3dblockcache.h"
#include "z3dtexture.h"
#include "z3dvolume.h"
#include "zbbox.h"
#include <QObject>
#include <set>

namespace nim {

class Z3DShaderProgram;

// Z3DVolume coordinates:
// 1. Voxel Coordinate:    [0, dim.x-1] x [0, dim.y-1] x [0, dim.z-1]
//                     in which (0,0,0) is LeftUpFront Corner (LUF)
//                         and dim-1 is RightDownBack Corner (RDB)
// 2. Texture Coordinate:  [0.0, 1.0] x [0.0, 1.0] x [0.0, 1.0]
// might throw exception
class Z3DImg : public QObject
{
Q_OBJECT
public:
  // Z3DVolume will take ownership of the img
  Z3DImg(const ZImgPack& imgPack, const glm::vec3& scale, QObject* parent = nullptr);

  bool is2DData() const
  { return m_imgPack.imgInfo().depth == 1; }

  bool is3DData() const
  { return m_imgPack.imgInfo().depth > 1; }

  glm::uvec3 dimensions() const
  { return glm::uvec3(m_imgPack.imgInfo().width, m_imgPack.imgInfo().height, m_imgPack.imgInfo().depth); }

  size_t numChannels() const
  { return m_nChannels; }

  col4 channelColor(size_t c) const
  { return m_imgPack.imgInfo().channelColors[c]; }

  bool isVolumeDownsampled() const
  { return m_isVolumeDownsampled; }

  const std::vector<std::unique_ptr<Z3DVolume>>& volumes() const
  { return m_volumes; }

  static glm::uvec3 imageBlockSize()
  { return glm::uvec3(30, 30, 30); }

  // Returns a string representation of the sampler type: "sampler2D" for 2D image, "sampler3D" for 3D volume
  QString samplerType() const;

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  glm::vec3 physicalLUF() const
  { return glm::vec3(0, 0, 0); }

  glm::vec3 physicalRDB() const
  { return glm::max(glm::vec3(1, 1, 1),
                    glm::vec3(m_imgPack.imgInfo().width - 1,
                              m_imgPack.imgInfo().height - 1,
                              m_imgPack.imgInfo().depth - 1)); }

  glm::vec3 physicalLDF() const
  { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalLUF().z); }

  glm::vec3 physicalRDF() const
  { return glm::vec3(physicalRDB().x, physicalRDB().y, physicalLUF().z); }

  glm::vec3 physicalRUF() const
  { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalLUF().z); }

  glm::vec3 physicalLUB() const
  { return glm::vec3(physicalLUF().x, physicalLUF().y, physicalRDB().z); }

  glm::vec3 physicalLDB() const
  { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalRDB().z); }

  glm::vec3 physicalRUB() const
  { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalRDB().z); }

  std::vector<std::unique_ptr<Z3DVolume>> makeXSliceVolume(size_t x);

  std::vector<std::unique_ptr<Z3DVolume>> makeYSliceVolume(size_t y);

  std::vector<std::unique_ptr<Z3DVolume>> makeZSliceVolume(size_t z);

  ZBBox<glm::dvec3> physicalBoundBox() const
  { return ZBBox<glm::dvec3>(glm::dvec3(physicalLUF()), glm::dvec3(physicalRDB())); }

  void setScale(const glm::vec3& scale);

  size_t numLevels() const
  { return m_numLevels; }

  void bindFullResBlockIDsShader(Z3DShaderProgram& shader) const;

  void bindFullResRenderShader(Z3DShaderProgram& shader) const;

  void bindImageCacheToFullResRenderShader(Z3DShaderProgram& shader, size_t c) const;

  bool updateAndUploadPageDirectoryCaches(const std::set<uint32_t>& missingBlockIDs,
                                          const std::set<uint32_t>& usedBlockIDs);

  void uploadImageCache(size_t channel);

protected:
  void readVolumes();

  void checkPageSystemError();

protected:
  glm::uvec3 m_pageTableBlockSize;
  glm::uvec3 m_pageTableCacheNumBlocks;
  glm::uvec3 m_imageBlockSize;
  glm::ivec3 m_imageBlockReadSize;
  glm::uvec3 m_imageCacheNumBlocks;
  int m_unmappedFlag = 0;  // 1 - 32*32*32(32768) means number of blocks mapped
  int m_emptyFlag = 40000;

  std::vector<glm::ivec4> m_pageDirectory;
  glm::ivec3 m_pageDirectorySize;
  std::unique_ptr<Z3DTexture> m_pageDirectoryTexture;
  std::vector<glm::ivec4> m_pageTableCache;
  glm::ivec3 m_pageTableCacheSize;
  std::unique_ptr<Z3DTexture> m_pageTableCacheTexture;
  std::unique_ptr<Z3DBlockCache<glm::ivec4>> m_pageTableCacheManager;
  std::vector<std::unique_ptr<Z3DTexture>> m_imageCacheTextures;
  std::unique_ptr<Z3DBlockCache<glm::ivec4>> m_imageCacheManager;

  size_t m_numLevels = 1;
  std::vector<glm::ivec3> m_pageDirectoryBases;
  std::vector<glm::uvec3> m_pageDirectoryDimensions;
  std::vector<glm::uvec3> m_pageTableDimensions;
  std::vector<glm::uvec3> m_imageDimensions;
  std::vector<glm::uvec3> m_imageBounds;
  std::vector<glm::uvec3> m_levelScales;
  std::vector<glm::uvec4> m_posToBlockIDs;

  std::vector<glm::vec3> m_voxelWorldDimensions;
  std::vector<float> m_voxelWorldSizes;

private:
  //std::unique_ptr<Z3DImgHistogramThread> m_histogramThread;
  const ZImgPack& m_imgPack;
  std::vector<std::unique_ptr<Z3DVolume>> m_volumes;
  size_t m_nChannels = 0;
  bool m_isVolumeDownsampled;

  std::vector<std::vector<std::pair<glm::uvec3, glm::ivec4>>> m_channelPendingUpdates;  // block cache pos and block image pos
};

} // namespace nim

