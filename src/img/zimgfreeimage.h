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
  virtual bool supportRead() const override;

  virtual bool supportWrite() const override;

  virtual QString shortName() const override;

  virtual QString fullName() const override;

  virtual QStringList extensions() const override;

  virtual FileFormat format() const override
  { return FileFormat::FreeImage; }

  virtual void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                        std::vector<std::set<size_t>>* pyramidalRatios) override;

  virtual void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  virtual void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  virtual void
  readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio) override;

  void readInfo(uint8_t* mem, size_t size, ZImgInfo& info);

  void readImg(uint8_t* mem, size_t size, uint8_t* des, size_t desSize);
};

} // namespace nim

