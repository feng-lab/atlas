#pragma once

#include "zimgformat.h"

namespace nim {

#define HACK_HDF5

#ifdef HACK_HDF5
struct HDF5ChunkInfo {
  size_t offset = 0;
  size_t length = 0;
  bool compressed = true;
};
#endif

class ZImgHDF5SubBlock : public ZImgSubBlock
{
public:
  ZImgHDF5SubBlock(QString fileName,
                   std::vector<std::string> tiles,
                   const ZImgInfo& info,
                   size_t ratio_, size_t t_, size_t z_, size_t x_, size_t y_);

  [[nodiscard]] std::shared_ptr<ZImg> read() const override;

  [[nodiscard]] ZImgInfo readInfo() const override;

#ifdef HACK_HDF5
  void setHDF5ChunkInfos(const std::vector<HDF5ChunkInfo>& cinfos)
  { m_hdf5Tiles = cinfos; }
#endif

protected:
  QString m_filename;
  std::vector<std::string> m_tiles;  // cat in Dimension::C
  ZImgInfo m_info;
  size_t m_ratio;
  size_t m_x;
  size_t m_y;

#ifdef HACK_HDF5
  std::vector<HDF5ChunkInfo> m_hdf5Tiles;
#endif
};

class ZImgHDF5 : public ZImgFormat
{
public:

  // ZImgFormat interface
public:
  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  { return FileFormat::HDF5Img; }

  void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene,
               size_t xRatio, size_t yRatio, size_t zRatio) override;

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider,
                const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename, const ZImgBlockProvider& imgBlockrovider,
                const ZImgWriteParameters& paras) override;

  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;
};

} // namespace nim



