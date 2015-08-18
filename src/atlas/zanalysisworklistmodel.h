#ifndef ZANALYSISWORKLISTMODEL_H
#define ZANALYSISWORKLISTMODEL_H

#include "zgenerateanalysistextfile.h"
#include <QAbstractTableModel>
#include <QVariant>
#include <QIODevice>
#include <QChar>
#include <QString>
#include <QStringList>
#include <QModelIndex>
#include <QMimeData>
#include <map>
class QTextCodec;

namespace nim {

class ZAnalysisWorklistModel : public QAbstractTableModel
{
  Q_OBJECT
public:
  explicit ZAnalysisWorklistModel(QObject *parent = 0);
  explicit ZAnalysisWorklistModel(QIODevice *file, QObject *parent = 0);
  explicit ZAnalysisWorklistModel(const QString& filename, QObject *parent = 0);
  ~ZAnalysisWorklistModel();

  void setSource(QIODevice *file, QTextCodec* codec = 0);
  void setSource(const QString& filename, QTextCodec* codec = 0);

  void toCSV(QIODevice *file, bool withHeader = true, QChar separator = ',', QTextCodec* codec = 0) const;
  void toCSV(const QString filename, bool withHeader = true, QChar separator = ',', QTextCodec* codec = 0) const;

  int rowCount(const QModelIndex& parent = QModelIndex()) const;
  int columnCount(const QModelIndex& parent = QModelIndex()) const;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;
  bool setData(const QModelIndex& index, const QVariant& data, int role = Qt::EditRole);
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;
  Qt::ItemFlags flags(const QModelIndex& index) const;

  void reset();

  const QList<ZAnalysisTextFileInput>& worklist() const { return m_inputs; }

  QStringList mimeTypes() const;
  Qt::DropActions supportedDropActions() const;
  bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent);

protected:
  QStringList m_header;
  size_t m_rowCount;
  QList<ZAnalysisTextFileInput> m_inputs;
  std::map<size_t, ZAnalysisTextFileInput*> m_rowToInput;
};

} // namespace nim

#endif // ZANALYSISWORKLISTMODEL_H
