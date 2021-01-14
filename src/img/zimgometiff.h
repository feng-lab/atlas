#pragma once

#include "zimgtiff.h"
#include <map>

class QXmlStreamReader;

namespace nim {

class ZImgOmeTiff : public ZImgTiff
{
public:

  // ZImgFormat interface
public:
  QString shortName() const override;

  QString fullName() const override;

  QStringList extensions() const override;

  FileFormat format() const override
  { return FileFormat::OmeTiff; }

  void writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras) override;

  void writeImg(const QString& filename, const ZImgSliceProvider& imgSliceProvider,
                const ZImgWriteParameters& paras) override;

  bool supportRead() const override;

  bool supportWrite() const override;

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

  void parsePixels(QXmlStreamReader& xml, ZTiff& tiff);

  void parseTiffData(QXmlStreamReader& xml, ZTiff& tiff);

  void parseChannel(QXmlStreamReader& xml);

  //
  static QString createOmeXml(const ZImgInfo& info, const QString& dimensionOrder);

protected:
  struct IFDPos
  {
    IFDPos()
      : z(std::numeric_limits<size_t>::max())
      , c(std::numeric_limits<size_t>::max())
      , t(std::numeric_limits<size_t>::max())
    {}

    IFDPos(size_t z_, size_t c_, size_t t_)
      : z(z_), c(c_), t(t_)
    {}

    size_t z, c, t;
  };

  ZImgInfo m_omeImgInfo;
  std::map<size_t, IFDPos> m_ifdIdxPosMap;
};

}  // namespace nim

