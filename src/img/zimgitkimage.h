#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgITKImage : public ZImgFormat
{
public:
  ZImgITKImage();

  // ZImgFormat interface
public:
  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::ITKImage;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename,
               ZImg& img,
               const ZImgRegion& region,
               size_t scene,
               const ZImgReadOptions& readOptions = ZImgReadOptions::complete()) override;

  void checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;
};

} // namespace nim
