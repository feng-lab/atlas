#pragma once

#include "zimgformat.h"

namespace itk {
class ImageIOBase;
} // namespace itk

namespace nim {

class ZImgITKImage : public ZImgFormat
{
public:
  ZImgITKImage();

  // ZImgFormat interface

public:
  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  {
    return FileFormat::ITKImage;
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

  bool supportRead() const override;

  bool supportWrite() const override;

protected:
  void parseInfo(const itk::ImageIOBase* imageIO, ZImgInfo& info, bool isNd2);

  void parseMetadata(const itk::ImageIOBase* imageIO, ZImgMetadata& meta);

  bool hasSCIFIOSupport() const;
};

} // namespace nim
