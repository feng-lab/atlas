#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgHDF5SubBlock : public ZImgSubBlock
{
public:
  ZImgHDF5SubBlock(const QString& fileName, const ZImgInfo& info,
                   size_t ratio_, size_t t_, size_t z_, size_t x_, size_t y_);

  virtual std::shared_ptr<ZImg> read() const override;

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
  virtual QString shortName() const override;

  virtual QString fullName() const override;

  virtual QStringList extensions() const override;

  virtual FileFormat format() const override
  { return FileFormat::HDF5Img; }

  virtual void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                        std::vector<std::set<size_t>>* pyramidalRatios) override;

  virtual void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  virtual void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  virtual void
  readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio) override;

  virtual void writeImg(const QString& filename, const ZImg& img, Compression comp) override;

  virtual void writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider, Compression comp) override;

  virtual bool supportRead() const override;

  virtual bool supportWrite() const override;
};

} // namespace nim



