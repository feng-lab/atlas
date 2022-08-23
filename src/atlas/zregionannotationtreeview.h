#pragma once

#include "zdoc.h"
#include "zregionannotationtreemodel.h"
#include <QTreeView>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class ZRegionAnnotationTreeView : public QTreeView
{
  Q_OBJECT

public:
  ZRegionAnnotationTreeView(ZRegionAnnotationTreeModel& objModel,
                            ZRegionAnnotationPack& rap,
                            ZDoc& doc,
                            QWidget* parent = nullptr);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void buttonClickedForUserData(const QVariant& ud);

  void keyPressEvent(QKeyEvent* e) override;

  void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;

  void createContextMenu();

private:
  ZRegionAnnotationTreeModel& m_ratModel;
  ZRegionAnnotationPack& m_regionAnnotationPack;
  ZDoc& m_doc;
  QSortFilterProxyModel* m_ratProxyModel;
  QMenu* m_contextMenu;
};

} // namespace nim
