#pragma once

#include "zimgformat.h"

namespace nim {

class ZTiff;

class ZImgTiff : public ZImgFormat
{
public:
  // ZImgFormat interface

public:
  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::Tiff;
  }

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene) override;

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename,
                const ZImgSliceProvider& imgSliceProvider,
                const ZImgWriteParameters& paras) override;

  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;

protected:
  // default read into ZTiff and detect ZImgInfo, inherit this to read more information
  virtual void readIntoInternalStructure(const QString& filename, ZTiff& tiff);

  // called before each read, inherit this if subclass has internal information
  virtual void clearInternalState();

  // from internal structure to m_imgInfo
  virtual void detectImgInfo(ZTiff& tiff);

  // from internal structure
  void readMetadataInternal(ZImgMetadata& meta, size_t scene, ZTiff& tiff);

  // from internal structure
  void readThumbnailInternal(ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene, ZTiff& tiff);

  // image order
  // default is 0
  void setStartIFDIndex(size_t s)
  {
    m_startIFDIndex = s;
  }

  // must be 3 or 4 characters. default is "ZTL". if it contains "C" then channels are in different IFD.
  void setDimensionOrder(const QString& order);

  // map image ifd index to multidimensional image, if ifd contains all channel, set c to -1
  // index is index of idf of all normal images (not downsampled image)
  // return true if ifd should be read. if ifd doesn't belong to img (e.g. ome-tiff), return false
  // default use start ifd index and dimension order to calculate correct positioin
  virtual bool mapIFDToImgLocation(size_t ifdIdx, index_t& z, index_t& c, index_t& t, index_t& l);

  // utils function
  static bool isDimensionOrderValid(const QString& order);

  static bool IFDToLoc(size_t ifdIdx,
                       index_t& z,
                       index_t& c,
                       index_t& t,
                       index_t& l,
                       size_t startIFDIndex,
                       const QString& dimensionOrder,
                       const ZImgInfo& imgInfo,
                       size_t numScenes,
                       index_t startZ = 0,
                       index_t startC = 0,
                       index_t startT = 0,
                       index_t startL = 0);

protected:
  std::vector<ZImgInfo> m_imgInfo;
  QString m_dimensionOrder = "ZTL";
  size_t m_startIFDIndex = 0;
  std::string m_imageDescription;

private:
  bool m_isImageJTiff = false;
  bool m_onlyOneIFDInImageJTiff = false;
};

} // namespace nim
