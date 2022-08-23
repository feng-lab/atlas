#pragma once

#include "zswcpack.h"
#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <map>

namespace nim {

class ZDoc;

class ZSwcTreeModel : public QAbstractItemModel
{
  Q_OBJECT

public:
  enum Column
  {
    IDColumn,
    TypeColumn,
    XColumn,
    YColumn,
    ZColumn,
    RadiusColumn,
    TopologyColumn,
    ParentIDColumn,
    LabelColumn,
    ColumnCount
  };

  explicit ZSwcTreeModel(ZSwcPack& swcPack, ZDoc& doc, QObject* parent = nullptr);

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

  [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent) const override;

  [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  void clicked(const QModelIndex& idxIn);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

  void updateModel();

protected:
  ZSwcPack& m_swcPack;
  ZDoc& m_doc;
};

} // namespace nim
