#pragma once

#include "zimg.h"
#include <span>

// don't use ZImgFormat and derived class directly, use ZImgIO::instance() instead

namespace nim {

class ZImgSliceProvider;

class ZImgBlockProvider;

class ZImgFormat
{
public:
  virtual ~ZImgFormat();

  // whether read or write is supported
  [[nodiscard]] virtual bool supportRead() const = 0;

  [[nodiscard]] virtual bool supportWrite() const = 0;

  // check file extension
  [[nodiscard]] virtual bool canRead(const QString& filename) const;

  [[nodiscard]] bool canWrite(const QString& filename) const;

  [[nodiscard]] virtual QString shortName() const = 0;

  [[nodiscard]] virtual QString fullName() const = 0;

  // should start with '.'
  [[nodiscard]] virtual QStringList extensions() const = 0;

  [[nodiscard]] virtual FileFormat format() const = 0;

  // following io functions will throw ZException if read or write fails

  // only info, input can be changed even if read failed
  virtual void readInfo(const QString& filename,
                        std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) = 0;

  // only metadata, input can be changed even if read failed
  virtual void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) = 0;

  // only thumbnail, input can be changed even if read failed
  // for thumbnail, x and y range has no effect
  virtual void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) = 0;

  // read image data, input can be changed even if read failed.
  // The default options read a complete ZImg container, including descriptive
  // metadata and thumbnails when the format supports them.
  virtual void readImg(const QString& filename,
                       ZImg& img,
                       const ZImgRegion& region,
                       size_t scene,
                       const ZImgReadOptions& readOptions = ZImgReadOptions::complete())
  {
    readImg(filename, img, region, scene, 1, 1, 1, readOptions);
  }

  virtual void readImg(const QString& filename,
                       ZImg& img,
                       const ZImgRegion& region,
                       size_t scene,
                       size_t xRatio,
                       size_t yRatio,
                       size_t zRatio,
                       const ZImgReadOptions& readOptions = ZImgReadOptions::complete())
  {
    CHECK(xRatio >= 1 && yRatio >= 1 && zRatio >= 1);
    readImg(filename, img, region, scene, readOptions);
    if (xRatio > 1 || yRatio > 1 || zRatio > 1) {
      img.zoom(1.0 / xRatio, 1.0 / yRatio, 1.0 / zRatio);
    }
  }

  // check whether the current img can be represented by the file with the paras
  // base calss only check filename, subclass should do more, throw ioexception with reason
  // should be used in writeImg functions
  virtual void checkImgBeforeWriting(const QString& filename, const ZImgInfo& info, const ZImgWriteParameters& paras);

  virtual void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras);

  virtual void
  writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider, const ZImgWriteParameters& paras);

  virtual void
  writeImg(const QString& filename, const ZImgBlockProvider& imgBlockProvider, const ZImgWriteParameters& paras);

  // convert RGBARGBA..... to RRR...GGG...BBB...AAA...
  static void CXYZtoXYZC(const ZImg& bufImg, ZImg& img, bool BGRtoRGB = false, bool ARGBtoRGBA = false);

  // convert RRR...GGG...BBB...AAA... to RGBARGBA.....
  static void XYZCtoCXYZ(const ZImg& bufImg, ZImg& img);

  // convert bufImg of dimensionOrder to img of dimensionOrder XYZCT
  static void fixDimensionOrder(const uint8_t* buf, const QString& dimensionOrder, ZImg& img, bool BGRtoRGB = false);

protected:
  ZImg readRawImg(const QString& filename,
                  const ZImgInfo& imgInfo,
                  const QString& dimensionOrder,
                  size_t dataOffset,
                  const ZImgRegion& region,
                  size_t timeStride = 0);

  // Canonical API: accept strides as a span (expects 5 elements in XYZCT order).
  ZImg readRawImg(const QString& filename,
                  const ZImgInfo& imgInfo,
                  std::span<const size_t> dimensionStrides,
                  size_t dataOffset,
                  const ZImgRegion& region);

  static void createDefaultSubBlocks(const QString& filename,
                                     const std::vector<ZImgInfo>& infos,
                                     std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                                     FileFormat format = FileFormat::Unknown);

  static void createEmptySubBlocks(const std::vector<ZImgInfo>& infos,
                                   std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks);

  // Create XY-tiled sub-blocks (e.g., 512x512) for every Z and T slice.
  // Useful for formats that support efficient random-access region reads.
  static void createTiledSubBlocks(const QString& filename,
                                   const std::vector<ZImgInfo>& infos,
                                   std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                                   size_t tileWidth = 512,
                                   size_t tileHeight = 512,
                                   FileFormat format = FileFormat::Unknown);
};

} // namespace nim
