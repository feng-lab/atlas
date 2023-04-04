#pragma once

#include "z3dgl.h"
#include "zimgpack.h"
#include "z3dblockcache.h"
#include "z3dtexture.h"
#include "z3dvolume.h"
#include "zvertexbufferobject.h"
#include "zbbox.h"
#include <folly/CancellationToken.h>
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
  Z3DImg(const ZImgPack& imgPack,
         const glm::vec3& scale,
         const std::vector<glm::dvec2>& displayRanges,
         QObject* parent = nullptr);

  [[nodiscard]] const ZImgPack& imgPack() const
  {
    return m_imgPack;
  }

  [[nodiscard]] bool is2DData() const
  {
    return m_imgPack.imgInfo().depth == 1;
  }

  [[nodiscard]] bool is3DData() const
  {
    return m_imgPack.imgInfo().depth > 1;
  }

  [[nodiscard]] glm::dvec2 displayRange(size_t ch) const
  {
    return m_channelDisplayRanges.at(ch);
  }

  [[nodiscard]] glm::uvec3 dimensions() const
  {
    return glm::uvec3(m_imgPack.imgInfo().width, m_imgPack.imgInfo().height, m_imgPack.imgInfo().depth);
  }

  [[nodiscard]] size_t numChannels() const
  {
    return m_nChannels;
  }

  [[nodiscard]] col4 channelColor(size_t c) const
  {
    return m_imgPack.imgInfo().channelColors[c];
  }

  [[nodiscard]] bool isVolumeDownsampled() const
  {
    return m_isVolumeDownsampled;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<Z3DVolume>>& volumes() const
  {
    return m_volumes;
  }

  // Returns a string representation of the sampler type: "sampler2D" for 2D image, "sampler3D" for 3D volume
  [[nodiscard]] QString samplerType() const;

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  [[nodiscard]] glm::vec3 physicalLUF() const
  {
    return glm::vec3(0, 0, 0);
  }

  [[nodiscard]] glm::vec3 physicalRDB() const
  {
    return glm::max(
      glm::vec3(1, 1, 1),
      glm::vec3(m_imgPack.imgInfo().width - 1, m_imgPack.imgInfo().height - 1, m_imgPack.imgInfo().depth - 1));
  }

  [[nodiscard]] glm::vec3 physicalLDF() const
  {
    return {physicalLUF().x, physicalRDB().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalRDF() const
  {
    return {physicalRDB().x, physicalRDB().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalRUF() const
  {
    return {physicalRDB().x, physicalLUF().y, physicalLUF().z};
  }

  [[nodiscard]] glm::vec3 physicalLUB() const
  {
    return {physicalLUF().x, physicalLUF().y, physicalRDB().z};
  }

  [[nodiscard]] glm::vec3 physicalLDB() const
  {
    return {physicalLUF().x, physicalRDB().y, physicalRDB().z};
  }

  [[nodiscard]] glm::vec3 physicalRUB() const
  {
    return {physicalRDB().x, physicalLUF().y, physicalRDB().z};
  }

  std::vector<std::unique_ptr<Z3DVolume>> makeXSliceVolume(size_t x);

  std::vector<std::unique_ptr<Z3DVolume>> makeYSliceVolume(size_t y);

  std::vector<std::unique_ptr<Z3DVolume>> makeZSliceVolume(size_t z);

  [[nodiscard]] ZBBox<glm::dvec3> physicalBoundBox() const
  {
    return ZBBox<glm::dvec3>(glm::dvec3(physicalLUF()), glm::dvec3(physicalRDB()));
  }

  void setScale(const glm::vec3& scale);

  void setChannelDisplayRanges(const std::vector<glm::dvec2>& displayRanges);

  [[nodiscard]] size_t numLevels() const
  {
    return m_numLevels;
  }

  [[nodiscard]] size_t numCachedImages(size_t c) const
  {
    return m_channelImageCacheManagers[c]->size();
  }

  void bindFullResBlockIDsShader(Z3DShaderProgram& shader, size_t c) const;

  void bindFullResRenderShader(Z3DShaderProgram& shader, size_t c) const;

  bool updateAndUploadPageDirectoryCaches(const std::vector<uint32_t>& missingBlockIDs,
                                          const std::vector<uint32_t>& usedBlockIDs,
                                          size_t c,
                                          folly::CancellationToken cancellationToken,
                                          bool silenceExistingWarning = true);

Q_SIGNALS:

  void renderingError(const QString& error) const;

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
  const int m_unmappedFlag = 0; // 1 - 32*32*32(32768) means number of blocks mapped
  const int m_emptyFlag = 40000;

  std::vector<std::vector<glm::uvec4>> m_channelPageDirectories;
  glm::uvec3 m_pageDirectorySize;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelPageDirectoryTextures;
  std::vector<std::vector<glm::uvec4>> m_channelPageTableCaches;
  glm::uvec3 m_pageTableCacheSize;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelPageTableCacheTextures;
  std::vector<std::unique_ptr<Z3DBlockCache<glm::uvec4>>> m_channelPageTableCacheManagers;
  std::vector<std::unique_ptr<Z3DTexture>> m_channelImageCacheTextures;
  std::vector<std::unique_ptr<Z3DBlockCache<glm::uvec4>>> m_channelImageCacheManagers;

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
  glm::vec3 m_volumeVoxelWorldDimension;
  float m_volumeVoxelWorldSize;

private:
  // std::unique_ptr<Z3DImgHistogramThread> m_histogramThread;
  const ZImgPack& m_imgPack;
  std::vector<std::unique_ptr<Z3DVolume>> m_volumes;
  size_t m_nChannels = 0;
  bool m_isVolumeDownsampled;

  ZVertexBufferObject m_PBO;

  std::vector<glm::dvec2> m_channelDisplayRanges;
};

} // namespace nim
