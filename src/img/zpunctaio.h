#pragma once

#include <QStringList>

namespace nim {

class ZPuncta;

class ZPunctaIO
{
public:
  static ZPunctaIO& instance();

  ZPunctaIO();

  bool canReadFile(const QString& filename);

  bool canWriteFile(const QString& filename);

  const QString& getQtReadNameFilter() const
  { return m_readFilter; }

  void getQtWriteNameFilter(QStringList& filters, QStringList& formats);

  void load(const QString& filename, ZPuncta& puncta) const;

  void save(const ZPuncta& puncta, const QString& filename, QString format) const;

private:
  static void readNimpFile(const QString& filename, ZPuncta& puncta) ;

  static void writeNimpFile(const ZPuncta& puncta, const QString& filename) ;

  static void readV3DApoFile(const QString& file, ZPuncta& puncta) ;

  static void writeV3DApoFile(const ZPuncta& puncta, const QString& file) ;

  static void readV3DMarkerFile(const QString& file, ZPuncta& puncta) ;

  static void readMatFile(const QString& file, ZPuncta& puncta) ;

  static void writeMatFile(const ZPuncta& puncta, const QString& file) ;

private:
  QStringList m_readExts;
  QStringList m_writeExts;
  QString m_readFilter;
  QStringList m_writeFilters;
  QStringList m_writeFormats;
};

} // namespace nim

