#pragma once

#include "zregionannotation.h"
#include "z3dmeshfilter.h"
#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <map>

namespace nim {

class Z3DRegionAnnotationViewSettingTreeModel : public QAbstractItemModel
{
Q_OBJECT
public:
  enum Column
  {
    AbbreviationColumn, WidgetColumn, ColumnCount
  };

  explicit Z3DRegionAnnotationViewSettingTreeModel(
    ZRegionAnnotation& anno,
    std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
    QObject* parent = nullptr);

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

  void clicked(const QModelIndex& index);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode>& m_annotationTree;
  std::map<RegionNode*, ZTree<RegionNode>::Iterator> m_nodeToIter;
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& m_idToMeshFilters;
  std::unique_ptr<QWidget> m_regionViewSettingEditorWindow;
};

} // namespace nim

