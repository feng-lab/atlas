#pragma once

#include "zregionannotation.h"
#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <map>

namespace nim {

class ZROIFilter;

class Z3DMeshFilter;

class ZRegionAnnotationViewSettingTreeModel : public QAbstractItemModel
{
  Q_OBJECT

public:
  enum Column
  {
    AbbreviationColumn,
    WidgetColumn,
    ColumnCount
  };

  ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno,
                                        std::map<int64_t, std::unique_ptr<ZROIFilter>>& idToROIFilters,
                                        QObject* parent = nullptr);

  ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno,
                                        std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
                                        QObject* parent = nullptr);

  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

  [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

  bool setData(const QModelIndex& index, const QVariant& value, int role) override;

  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

  [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent) const override;

  [[nodiscard]] QModelIndex parent(const QModelIndex& index) const override;

  [[nodiscard]] int rowCount(const QModelIndex& parent) const override;

  [[nodiscard]] int columnCount(const QModelIndex& parent) const override;

  void clicked(const QModelIndex& index);

  void doubleClicked(const QModelIndex& index);

  void activated(const QModelIndex& idxIn);

private:
  explicit ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno, QObject* parent = nullptr);

protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode>& m_annotationTree;
  std::map<RegionNode*, ZTree<RegionNode>::Iterator> m_nodeToIter;
  std::map<int64_t, std::unique_ptr<ZROIFilter>>* m_idToROIFilters = nullptr;
  std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>* m_idToMeshFilters = nullptr;
  std::unique_ptr<QWidget> m_regionViewSettingEditorWindow;
};

} // namespace nim
