#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgBioFormats : public ZImgFormat
{
public:
  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;

  [[nodiscard]] bool canRead(const QString& filename) const override;

  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::BioFormats;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene) override;
};

} // namespace nim
