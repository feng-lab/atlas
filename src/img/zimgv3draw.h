#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgV3DRaw : public ZImgFormat
{
public:

  // ZImgFormat interface
public:
  virtual QString shortName() const override;

  virtual QString fullName() const override;

  virtual QStringList extensions() const override;

  virtual FileFormat format() const override
  { return FileFormat::Vaa3DRaw; }

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

