#pragma once

#include "zimgformat.h"

#include <cstdint>
#include <span>

namespace nim {

class ZImgOpenImageIO : public ZImgFormat
{
public:
  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;

  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::OpenImageIO;
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

  static ZImgInfo readMemInfo(std::span<const uint8_t> bytes);

  static void readMemImg(std::span<const uint8_t> bytes, std::span<uint8_t> des);

  static void initializeRuntime();

  static void shutdownRuntime() noexcept;
};

} // namespace nim
