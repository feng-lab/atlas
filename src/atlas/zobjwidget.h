#ifndef ZOBJWIDGET_H
#define ZOBJWIDGET_H

#include <QTreeView>

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
  ZObjWidget(ZDoc* doc, ZObjModel* objModel, QItemSelectionModel* selectionModel, QWidget* parent = 0);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  virtual void keyPressEvent(QKeyEvent* e) override;

  void createContextMenu();

private:
  ZDoc* m_doc;
  ZObjModel* m_objModel;
  QSortFilterProxyModel* m_objProxyModel;
  QMenu* m_contextMenu;
};

} // namespace nim

#endif // ZOBJWIDGET_H
