#pragma once

#include "zimg.h"
#include <fstream>
#include <set>

// don't use ZImgFormat and derived class directly, use ZImgIOInstance instead

namespace nim {

class ZImgCommonSubBlock : public ZImgSubBlock
{
public:
  ZImgCommonSubBlock(const QString& fileName, FileFormat format, size_t scene, size_t ratio,
                     size_t t, size_t z, size_t x, size_t y, size_t width, size_t height);

  virtual std::shared_ptr<ZImg> read() const override;
  virtual ZImgInfo readInfo() const override;

protected:
  QString m_filename;
  FileFormat m_format;
  size_t m_scene;
  size_t m_t;
  size_t m_z;
  size_t m_x;
  size_t m_y;
  size_t m_width;
  size_t m_height;
};

class ZImgSliceProvider;

class ZImgFormat
{
public:
  virtual ~ZImgFormat();

  // whether read or write is supported
  virtual bool supportRead() const = 0;

  virtual bool supportWrite() const = 0;

  // check file extension
  bool canRead(const QString& filename) const;

  bool canWrite(const QString& filename) const;

  virtual QString shortName() const = 0;

  virtual QString fullName() const = 0;

  // should start with '.'
  virtual QStringList extensions() const = 0;

  virtual FileFormat format() const = 0;

  // following io functions will throw ZIOException if read or write fails

  // only info, input can be changed even if read failed
  virtual void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                        std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                        std::vector<std::set<size_t>>* pyramidalRatios) = 0;

  // only metadata, input can be changed even if read failed
  virtual void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) = 0;

  // only thumbnail, input can be changed even if read failed
  // for thumbnail, x and y range has no effect
  virtual void readThumbnail(const QString& filename, ZImgThumbernail& thumbnail,
                             const ZImgRegion& region, size_t scene) = 0;

  // read everything, input can be changed even if read failed
  virtual void readImg(const QString& filename, ZImg& img,
                       const ZImgRegion& region, size_t scene, size_t ratio) = 0;


  virtual void writeImg(const QString& filename, const ZImg& img, Compression comp);

  virtual void writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider, Compression comp);

  // convert RGBARGBA..... to RRR...GGG...BBB...AAA...
  static void CXYZtoXYZC(const ZImg& bufImg, ZImg& img, bool BGRtoRGB = false, bool ARGBtoRGBA = false);

  // convert RRR...GGG...BBB...AAA... to RGBARGBA.....
  static void XYZCtoCXYZ(const ZImg& bufImg, ZImg& img);

  // convert bufImg of dimensionOrder to img of dimensionOrder XYZCT
  static void fixDimensionOrder(const uint8_t* buf, const QString& dimensionOrder, ZImg& img, bool BGRtoRGB = false);

protected:
  ZImg readRawImg(const QString& filename, const ZImgInfo& imgInfo, const QString& dimensionOrder,
                  uint64_t dataOffset, const ZImgRegion& region);

  void createDefaultSubBlocks(const QString& filename, const std::vector<ZImgInfo>& infos,
                              std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                              std::vector<std::set<size_t>>* pyramidalRatios);

  void createEmptySubBlocks(const std::vector<ZImgInfo>& infos,
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                            std::vector<std::set<size_t>>* pyramidalRatios);
};

} // namespace nim

