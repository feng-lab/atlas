#pragma once

#include "zdoc.h"
#include "zroifilter.h"
#include "zregionannotationviewsettingtreemodel.h"
#include <QTreeView>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class ZRegionAnnotationViewSettingTreeView : public QTreeView
{
Q_OBJECT
public:
  ZRegionAnnotationViewSettingTreeView(ZRegionAnnotationViewSettingTreeModel& objModel,
                                       ZRegionAnnotation& anno,
                                       std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
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

  QSize minimumSizeHint() const override
  { return QSize(100, 300); }

  QSize sizeHint() const override
  { return QSize(300, 300); }

private:
  ZRegionAnnotationViewSettingTreeModel& m_ratModel;
  ZRegionAnnotation& m_regionAnnotation;
  QSortFilterProxyModel* m_ratProxyModel;
  QMenu* m_contextMenu;
  std::map<int, std::unique_ptr<ZROIFilter>>& m_idToROIFilters;
};

} // namespace nim

