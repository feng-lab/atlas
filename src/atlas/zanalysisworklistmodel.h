#pragma once

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
#include <QTextCodec>

namespace nim {

class ZAnalysisWorklistModel : public QAbstractTableModel
{
Q_OBJECT
public:
  explicit ZAnalysisWorklistModel(QObject* parent = 0);

  explicit ZAnalysisWorklistModel(const QString& filename, QObject* parent = 0);

  ~ZAnalysisWorklistModel();

  QString setSource(const QString& filename, QTextCodec* codec = QTextCodec::codecForName("UTF-8"));

  QString toCSV(const QString filename, bool withHeader = true, QChar separator = ',',
                QTextCodec* codec = QTextCodec::codecForName("UTF-8")) const;

  int rowCount(const QModelIndex& parent = QModelIndex()) const;

  int columnCount(const QModelIndex& parent = QModelIndex()) const;

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;

  bool setData(const QModelIndex& index, const QVariant& data, int role = Qt::EditRole);

  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;

  Qt::ItemFlags flags(const QModelIndex& index) const;

  void reset();

  const QList<ZAnalysisTextFileInput>& worklist() const
  { return m_inputs; }

  QStringList mimeTypes() const;

  Qt::DropActions supportedDropActions() const;

  bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent);

protected:
  QStringList m_header;
  size_t m_rowCount;
  QList<ZAnalysisTextFileInput> m_inputs;
  std::map<size_t, ZAnalysisTextFileInput*> m_rowToInput;
};

} // namespace nim

