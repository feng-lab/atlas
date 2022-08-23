#pragma once

#include "zpunctapack.h"
#include <QAbstractTableModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <vector>

namespace nim {

class ZPunctaTableModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  enum Column
  {
    ScoreColumn,
    XColumn,
    YColumn,
    ZColumn,
    RadiusColumn,
    VolSizeColumn,
    MassColumn,
    MeanIntensityColumn,
    MaxIntensityColumn,
    SDIntensityColumn,
    ColumnCount
  };

  explicit ZPunctaTableModel(ZPunctaPack& p, QObject* parent = nullptr);

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  void clicked(const QModelIndex& idxIn);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

  void updateModel();

protected:
  ZPunctaPack& m_puncta;
};

} // namespace nim
