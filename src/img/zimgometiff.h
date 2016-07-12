#ifndef ZIMGOMETIFF_H
#define ZIMGOMETIFF_H

#include "zimgtiff.h"
#include <map>

class QXmlStreamReader;

namespace nim {

class ZImgOmeTiff : public ZImgTiff
{
public:
  ZImgOmeTiff();

  // ZImgFormat interface
public:
  virtual QString shortName() const override;
  virtual QString fullName() const override;
  virtual QStringList extensions() const override;
  virtual FileFormat format() const override { return FileFormat::OmeTiff; }
  virtual void writeImg(const QString &filename, const ZImg &img, Compression comp) override;
  virtual void writeImg(const QString &filename, const ZImgSliceProvider &imgSliceProvider, Compression comp) override;
  virtual bool supportRead() const override;
  virtual bool supportWrite() const override;

  // ZImgTiff interface
protected:
  virtual void readIntoInternalStructure(const QString &filename, ZTiff &tiff) override;
  virtual void clearInternalState() override;
  virtual void detectImgInfo(ZTiff &tiff) override;
  virtual bool mapIFDToImgLocation(size_t ifdIdx, int &z, int &c, int &t, int &l) override;

protected:
  void readOmeInfo(ZTiff &tiff);
  void makeImageDescriptionTag(const ZImgInfo &info, const QString &dimensionOrder, ZImgMetatag &tag);

  //
  void parseOME(QXmlStreamReader &xml, ZTiff &tiff);
  void parsePixels(QXmlStreamReader &xml, ZTiff &tiff);
  void parseTiffData(QXmlStreamReader &xml, ZTiff &tiff);
  void parseChannel(QXmlStreamReader &xml);
  //
  static QString createOmeXml(const ZImgInfo &info, const QString &dimensionOrder);

protected:
  struct IFDPos {
    IFDPos()
      : z(std::numeric_limits<size_t>::max())
      , c(std::numeric_limits<size_t>::max())
      , t(std::numeric_limits<size_t>::max())
    {}
    IFDPos(size_t z, size_t c, size_t t)
      : z(z), c(c), t(t)
    {}
    size_t z, c, t;
  };

  ZImgInfo m_omeImgInfo;
  std::map<size_t, IFDPos> m_ifdIdxPosMap;
};

}  // namespace

#endif // ZIMGOMETIFF_H
