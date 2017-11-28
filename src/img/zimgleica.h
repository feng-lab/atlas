#pragma once

#include "zimgformat.h"
#include <QXmlStreamReader>

namespace nim {

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
  { return FileFormat::Leica; }

  void readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                std::vector<std::set<size_t>>* pyramidalRatios) override;

  void readMetadata(const QString& filename, ZImgMetadata& meta, size_t scene) override;

  void
  readThumbnail(const QString& filename, ZImgThumbernail& thumbnail, const ZImgRegion& region, size_t scene) override;

  void readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio) override;

private:
  void clearInternalState();

  void readXml(const QString& filename, QString& xml,
               std::vector<std::tuple<size_t, QString, size_t>>& memoryOffsetNameLength) const;

  void readLeicaInfo(const QString& xmlString);

  void parseMetadata(QXmlStreamReader& xml);

  void parseElement(QXmlStreamReader& xml);

  void parseReference(QXmlStreamReader& xml);

private:
  int m_version;
};

} // namespace



