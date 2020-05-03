#pragma once

#include "zregionannotation.h"
#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <map>

namespace nim {

class ZRegionAnnotationViewSettingTreeModel : public QAbstractItemModel
{
Q_OBJECT
public:
  enum Column
  {
    AbbreviationColumn, WidgetColumn, ColumnCount
  };

  explicit ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno, QObject* parent = nullptr);

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

  bool setData(const QModelIndex& index, const QVariant& value, int role) override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                    int role) const override;

  [[nodiscard]] QModelIndex index(int row, int column,
                                  const QModelIndex& parent) const override;

  [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  void clicked(const QModelIndex& idxIn);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode>& m_annotationTree;
  std::map<RegionNode*, ZTree<RegionNode>::Iterator> m_nodeToIter;
};

} // namespace nim

