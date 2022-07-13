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

  [[nodiscard]] const ZImgPack& imgPack() const
  { return m_imgPack; }

  [[nodiscard]] bool is2DData() const
  { return m_imgPack.imgInfo().depth == 1; }

  [[nodiscard]] bool is3DData() const
  { return m_imgPack.imgInfo().depth > 1; }

  [[nodiscard]] double minIntensity() const
  { return m_imgPack.minIntensity(); }

  [[nodiscard]] double maxsIntensity() const
  { return m_imgPack.maxIntensity(); }

  [[nodiscard]] glm::uvec3 dimensions() const
  { return glm::uvec3(m_imgPack.imgInfo().width, m_imgPack.imgInfo().height, m_imgPack.imgInfo().depth); }

  [[nodiscard]] size_t numChannels() const
  { return m_nChannels; }

  [[nodiscard]] col4 channelColor(size_t c) const
  { return m_imgPack.imgInfo().channelColors[c]; }

  [[nodiscard]] bool isVolumeDownsampled() const
  { return m_isVolumeDownsampled; }

  [[nodiscard]] const std::vector<std::unique_ptr<Z3DVolume>>& volumes() const
  { return m_volumes; }

  // Returns a string representation of the sampler type: "sampler2D" for 2D image, "sampler3D" for 3D volume
  [[nodiscard]] QString samplerType() const;

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  [[nodiscard]] glm::vec3 physicalLUF() const
  { return glm::vec3(0, 0, 0); }

  [[nodiscard]] glm::vec3 physicalRDB() const
  { return glm::max(glm::vec3(1, 1, 1),
                    glm::vec3(m_imgPack.imgInfo().width - 1,
                              m_imgPack.imgInfo().height - 1,
                              m_imgPack.imgInfo().depth - 1)); }

  [[nodiscard]] glm::vec3 physicalLDF() const
  { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalLUF().z); }

  [[nodiscard]] glm::vec3 physicalRDF() const
  { return glm::vec3(physicalRDB().x, physicalRDB().y, physicalLUF().z); }

  [[nodiscard]] glm::vec3 physicalRUF() const
  { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalLUF().z); }

  [[nodiscard]] glm::vec3 physicalLUB() const
  { return glm::vec3(physicalLUF().x, physicalLUF().y, physicalRDB().z); }

  [[nodiscard]] glm::vec3 physicalLDB() const
  { return glm::vec3(physicalLUF().x, physicalRDB().y, physicalRDB().z); }

  [[nodiscard]] glm::vec3 physicalRUB() const
  { return glm::vec3(physicalRDB().x, physicalLUF().y, physicalRDB().z); }

  std::vector<std::unique_ptr<Z3DVolume>> makeXSliceVolume(size_t x);

  std::vector<std::unique_ptr<Z3DVolume>> makeYSliceVolume(size_t y);

  std::vector<std::unique_ptr<Z3DVolume>> makeZSliceVolume(size_t z);

  [[nodiscard]] ZBBox<glm::dvec3> physicalBoundBox() const
  { return ZBBox<glm::dvec3>(glm::dvec3(physicalLUF()), glm::dvec3(physicalRDB())); }

  void setScale(const glm::vec3& scale);

  [[nodiscard]] size_t numLevels() const
  { return m_numLevels; }

  [[nodiscard]] size_t numCachedImages() const
  { return m_imageCacheManager->size(); }

  void bindFullResBlockIDsShader(Z3DShaderProgram& shader) const;

  void bindFullResRenderShader(Z3DShaderProgram& shader) const;

  void bindImageCacheToFullResRenderShader(Z3DShaderProgram& shader, size_t c) const;

  bool updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                          const std::vector<uint32_t>& usedBlockIDs,
                                          bool silenceExistingWarning = true);

  void uploadImageCache(size_t channel);

protected:
  void readVolumes();

  void checkPageSystemError();

protected:
  glm::uvec3 m_pageTableBlockSize = glm::uvec3(32, 32, 32);
  glm::uvec3 m_pageTableCacheNumBlocks;
  glm::uvec3 m_imageBlockSize = glm::uvec3(60, 60, 60);
  glm::uvec3 m_imageBlockSizePad = glm::uvec3(4, 4, 4);
  // glm::ivec3 m_imageBlockReadSize;
  glm::uvec3 m_imageCacheNumBlocks;
  const int m_unmappedFlag = 0;  // 1 - 32*32*32(32768) means number of blocks mapped
  const int m_emptyFlag = 40000;

  std::vector<glm::uvec4> m_pageDirectory;
  glm::uvec3 m_pageDirectorySize;
  std::unique_ptr<Z3DTexture> m_pageDirectoryTexture;
  std::vector<glm::uvec4> m_pageTableCache;
  glm::uvec3 m_pageTableCacheSize;
  std::unique_ptr<Z3DTexture> m_pageTableCacheTexture;
  std::unique_ptr<Z3DBlockCache<glm::uvec4>> m_pageTableCacheManager;
  std::vector<std::unique_ptr<Z3DTexture>> m_imageCacheTextures;
  std::unique_ptr<Z3DBlockCache<glm::uvec4>> m_imageCacheManager;

  size_t m_numLevels = 1;
  std::vector<glm::uvec3> m_pageDirectoryBases;
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

  std::vector<std::vector<std::pair<glm::uvec3, glm::uvec4>>> m_channelPendingUpdates;  // block cache pos and block image pos
};

} // namespace nim

