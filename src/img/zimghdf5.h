#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgHDF5SubBlock : public ZImgSubBlock
{
public:
  ZImgHDF5SubBlock(QString  fileName, const ZImgInfo& info,
                   size_t ratio_, size_t t_, size_t z_, size_t x_, size_t y_);

  std::shared_ptr<ZImg> read() const override;

  ZImgInfo readInfo() const override;

protected:
  QString m_filename;
  std::vector<std::string> m_tiles;  // cat in Dimension::C
  ZImgInfo m_info;
  size_t m_ratio;
  size_t m_x;
  size_t m_y;
};

class ZImgHDF5 : public ZImgFormat
{
public:

  // ZImgFormat interface
public:
  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
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

  bool supportRead() const override;

  bool supportWrite() const override;
};

} // namespace nim



