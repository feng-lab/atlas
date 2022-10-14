#pragma once

#include "zimgformat.h"

namespace nim {

class ZImgJpegXR : public ZImgFormat
{
public:
  static ZImgJpegXR& instance();

  // ZImgFormat interface

public:
  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;

  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::JpegXR;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene) override;

  void checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  static void readMemInfo(void* mem, size_t size, ZImgInfo& info);

  static void readMemImg(void* mem, size_t size, void* des, size_t desSize);

  static size_t writeImgToMem(const ZImg& img, const ZImgWriteParameters& paras, void* mem, size_t size);

protected:
  static void checkBeforeWriting(const ZImgInfo& info, const ZImgWriteParameters& paras);
};

} // namespace nim
