#pragma once

#include "zimgformat.h"
#include <QXmlStreamReader>
#include <QDir>

namespace nim {

struct ChannelDescription
{
  int32_t dataType;
  int32_t channelTag;
  uint32_t resolution;
  QString nameOfMeasuredQuantity;
  double min;
  double max;
  QString unit;
  QString LUTName;
  bool isLUTInverted;
  uint64_t bytesInc;
  uint32_t bitInc;
};

struct DimensionDescription
{
  int32_t dimID;
  uint32_t numberOfElements;
  double origin;
  double length;
  QString unit;
  uint64_t bytesInc;
  uint32_t bitInc;
};

struct ImageMemory
{
  uint64_t size = 0;
  QString memoryBlockID;
  QStringList fileNames;
  std::vector<uint64_t> fileSizes;
  std::vector<uint64_t> fileOffsets;
  uint64_t sceneOffset = 0;
};

struct ImageInfo
{
  std::vector<ChannelDescription> channels;
  std::vector<DimensionDescription> dimensions;
  std::vector<DimensionDescription> timeStampDimensions;
  std::vector<double> timeStamps;
  ImageMemory imageMemory;
};

struct DimensionInfo
{
  QString name;
  size_t start;
  size_t end;
  size_t stride;
};

class ZImgLeica : public ZImgFormat
{
public:
  // ZImgFormat interface

public:
  bool supportRead() const override;

  bool supportWrite() const override;

  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  {
    return FileFormat::Leica;
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

private:
  void clearInternalState();

  static int parseLIFVersion(const QString& xmlString);

  void readXml(const QString& filename,
               QString& xml,
               std::vector<std::tuple<size_t, QString, size_t>>& memoryOffsetNameLength) const;

  void readLeicaInfo(const QString& xmlString, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos);

  void parseMetadata(QXmlStreamReader& xml, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos);

  void parseElement(QXmlStreamReader& xml, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos);

  void parseXLIF(const QString& filename, std::vector<ImageInfo>& imageInfos);

  void parseXLCF(const QString& filename, std::vector<ImageInfo>& imageInfos);

  std::vector<ImageInfo> splitLeciaImageInfos(const std::vector<ImageInfo>& imageInfos);

  static std::vector<std::pair<size_t, size_t>>
  getMemoryRangeFromDimensionInfo(const std::vector<DimensionInfo>& dimensionInfos);

  static void detectInfos(std::vector<ZImgInfo>& infos, const std::vector<ImageInfo>& imageInfos);

private:
};

} // namespace nim
