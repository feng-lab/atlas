#pragma once

#include "zimgtiff.h"
#include <map>
#include <optional>

class QXmlStreamReader;

namespace nim {

class ZImgOmeTiff : public ZImgTiff
{
public:
  // ZImgFormat interface

public:
  [[nodiscard]] QString shortName() const override;

  [[nodiscard]] QString fullName() const override;

  [[nodiscard]] QStringList extensions() const override;

  [[nodiscard]] FileFormat format() const override
  {
    return FileFormat::OmeTiff;
  }

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename,
                const ZImgSliceProvider& imgSliceProvider,
                const ZImgWriteParameters& paras) override;

  void readInfo(const QString& filename,
                std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene) override;

  [[nodiscard]] bool supportRead() const override;

  [[nodiscard]] bool supportWrite() const override;

  // ZImgTiff interface

protected:
  void readIntoInternalStructure(const QString& filename, ZTiff& tiff) override;

  void clearInternalState() override;

  void detectImgInfo(ZTiff& tiff) override;

  bool mapIFDToImgLocation(size_t ifdIdx, index_t& z, index_t& c, index_t& t, index_t& l) override;

protected:
  void readOmeInfo(ZTiff& tiff);

  static void makeImageDescriptionTag(const ZImgInfo& info, const QString& dimensionOrder, ZImgMetatag& tag);

  //
  void parseOME(QXmlStreamReader& xml, ZTiff& tiff);

  bool parsePixels(QXmlStreamReader& xml, ZTiff& tiff, size_t seriesIndex);

  void parseTiffData(QXmlStreamReader& xml, ZTiff& tiff, size_t seriesIndex);

  void parseChannel(QXmlStreamReader& xml, size_t seriesIndex);

  void createOmeSubBlocks(const QString& filename,
                          ZTiff& tiff,
                          std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks) const;

  //
  static QString createOmeXml(const ZImgInfo& info, const QString& dimensionOrder);

protected:
  struct IFDPos
  {
    IFDPos()
      : z(std::numeric_limits<size_t>::max())
      , c(std::numeric_limits<size_t>::max())
      , t(std::numeric_limits<size_t>::max())
      , l(std::numeric_limits<size_t>::max())
    {}

    IFDPos(size_t z_, size_t c_, size_t t_, size_t l_)
      : z(z_)
      , c(c_)
      , t(t_)
      , l(l_)
    {}

    size_t z, c, t, l;
  };

  struct PlaneSource
  {
    QString filename;
    size_t ifdIndex = std::numeric_limits<size_t>::max();
    size_t z = 0;
    size_t c = 0;
    size_t t = 0;
    size_t channelCount = 1;
    bool valid = false;
  };

  struct SeriesInfo
  {
    ZImgInfo info;
    QString dimensionOrder = "ZCTL";
    size_t samplesPerPlane = 1;
    std::optional<size_t> significantBits;
    std::vector<PlaneSource> planes;
    bool metadataOnly = false;
  };

  std::vector<SeriesInfo> m_omeSeries;
  std::map<size_t, IFDPos> m_ifdIdxPosMap;
  QString m_currentFilename;
  QString m_omeXmlBaseFilename;
};

} // namespace nim
