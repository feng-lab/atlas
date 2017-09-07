#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgJpegXR : public ZImgFormat
{
public:
  static ZImgJpegXR& instance();

  // ZImgFormat interface
public:
  bool supportRead() const override;

  bool supportWrite() const override;

  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  { return FileFormat::JpegXR; }

  void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                std::vector<std::set<size_t>>* pyramidalRatios) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio) override;

  void readInfo(uint8_t* mem, size_t size, ZImgInfo& info);

  void readImg(uint8_t* mem, size_t size, uint8_t* des, size_t desSize);
};

} // namespace nim

