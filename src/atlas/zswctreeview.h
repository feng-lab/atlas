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

private:
  ZSwcTreeModel& m_ratModel;
  ZSwcPack& m_swcPack;
  ZDoc& m_doc;
  QSortFilterProxyModel* m_ratProxyModel;
};

} // namespace nim




