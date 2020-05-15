#pragma once

#include "zdoc.h"
#include "zswctreemodel.h"
#include <QTreeView>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class ZSwcTreeView : public QTreeView
{
Q_OBJECT
public:
  ZSwcTreeView(ZSwcTreeModel& objModel, ZSwcPack& swcPack, ZDoc& doc,
               QWidget* parent = nullptr);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void keyPressEvent(QKeyEvent* e) override;

  void selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) override;

  void onSwcSelectionChanged();

  void onSwcChanged();

  void onLockedStateChanged(bool l);

private:
  ZSwcTreeModel& m_ratModel;
  ZSwcPack& m_swcPack;
  ZDoc& m_doc;
  QSortFilterProxyModel* m_ratProxyModel;
  bool m_ignoreSelectionChangedSignal = false;
  bool m_skipSelectionChangedProcessing = false;
};

} // namespace nim




