#pragma once

#include <QTreeView>
#include <QTimer>

class QSortFilterProxyModel;

class QItemSelectionModel;

class QMenu;

namespace nim {

class ZDoc;

class ZObjModel;

class ZObjWidget : public QTreeView
{
  Q_OBJECT

public:
  ZObjWidget(ZDoc* doc, ZObjModel* objModel, QItemSelectionModel* selectionModel, QWidget* parent = nullptr);

protected:
  void showEvent(QShowEvent* event) override;

  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void keyPressEvent(QKeyEvent* e) override;

  void createContextMenu();

private:
  void scheduleAdaptColumns();

  void adaptColumnsIfPending();

  ZDoc* m_doc;
  ZObjModel* m_objModel;
  QSortFilterProxyModel* m_objProxyModel;
  QMenu* m_contextMenu;
  bool m_adaptColumnsPending = false;
  QTimer m_adaptColumnsTimer;
};

} // namespace nim
