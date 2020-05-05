#pragma once

#include "zdoc.h"
#include "z3dmeshfilter.h"
#include "z3dregionannotationviewsettingtreemodel.h"
#include <QTreeView>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class Z3DRegionAnnotationViewSettingTreeView : public QTreeView
{
Q_OBJECT
public:
  Z3DRegionAnnotationViewSettingTreeView(Z3DRegionAnnotationViewSettingTreeModel& objModel,
                                         ZRegionAnnotation& anno,
                                         std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
                                         QWidget* parent = nullptr);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void keyPressEvent(QKeyEvent* e) override;

  void createContextMenu();

  void wheelEvent(QWheelEvent* e) override;

private:
  Z3DRegionAnnotationViewSettingTreeModel& m_ratModel;
  ZRegionAnnotation& m_regionAnnotation;
  QSortFilterProxyModel* m_ratProxyModel;
  QMenu* m_contextMenu;
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& m_idToMeshFilters;
};

} // namespace nim

