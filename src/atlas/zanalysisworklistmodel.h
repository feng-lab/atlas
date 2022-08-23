#pragma once

#include "zgenerateanalysistextfile.h"
#include <QAbstractTableModel>
#include <QVariant>
#include <QIODevice>
#include <QChar>
#include <QStringList>
#include <QModelIndex>
#include <QMimeData>
#include <QStringConverter>
#include <map>

namespace nim {

class ZAnalysisWorklistModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  explicit ZAnalysisWorklistModel(QObject* parent = nullptr);

  explicit ZAnalysisWorklistModel(const QString& filename, QObject* parent = nullptr);

  QString setSource(const QString& filename, QStringConverter::Encoding encoding = QStringConverter::Utf8);

  [[nodiscard]] QString toCSV(const QString& filename,
                              bool withHeader = true,
                              QChar separator = ',',
                              QStringConverter::Encoding encoding = QStringConverter::Utf8) const;

  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;

  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

  bool setData(const QModelIndex& index, const QVariant& data, int role = Qt::EditRole) override;

  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

  void reset();

  [[nodiscard]] const std::vector<ZAnalysisTextFileInput>& worklist() const
  {
    return m_inputs;
  }

  [[nodiscard]] QStringList mimeTypes() const override;

  [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  bool
  dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

protected:
  QStringList m_header;
  size_t m_rowCount{};
  std::vector<ZAnalysisTextFileInput> m_inputs;
  std::map<size_t, ZAnalysisTextFileInput*> m_rowToInput;
};

} // namespace nim
