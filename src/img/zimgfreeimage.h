#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgFreeImage : public ZImgFormat
{
public:
  static ZImgFreeImage& instance();

  ZImgFreeImage();

  // ZImgFormat interface

public:
  bool supportRead() const override;

  bool supportWrite() const override;

  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  {
    return FileFormat::FreeImage;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene) override;

  static void readMemInfo(uint8_t* mem, size_t size, ZImgInfo& info);

  static void readMemImg(uint8_t* mem, size_t size, uint8_t* des, size_t desSize);
};

} // namespace nim
