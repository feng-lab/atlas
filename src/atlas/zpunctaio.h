#ifndef ZPUNCTAIO_H
#define ZPUNCTAIO_H

#include <QStringList>

namespace nim {

class ZPuncta;

#define ZPunctaIOInstance nim::ZPunctaIO::instance()

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
  void readNimpFile(const QString& file, ZPuncta& puncta) const;

  void writeNimpFile(const ZPuncta& puncta, const QString& file) const;

  void readV3DApoFile(const QString& file, ZPuncta& puncta) const;

  void writeV3DApoFile(const ZPuncta& puncta, const QString& file) const;

  void readV3DMarkerFile(const QString& file, ZPuncta& puncta) const;

  void readMatFile(const QString& file, ZPuncta& puncta) const;

  void writeMatFile(const ZPuncta& puncta, const QString& file) const;

private:
  QStringList m_readExts;
  QStringList m_writeExts;
  QString m_readFilter;
  QStringList m_writeFilters;
  QStringList m_writeFormats;
};

} // namespace nim

#endif // ZPUNCTAIO_H
