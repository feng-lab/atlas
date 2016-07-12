#ifndef ZREGIONANNOTATIONTREEVIEW_H
#define ZREGIONANNOTATIONTREEVIEW_H

#include <QTreeView>
#include "zregionannotationtreemodel.h"
#include "zdoc.h"

class QSortFilterProxyModel;
class QMenu;

namespace nim {

class ZRegionAnnotationTreeView : public QTreeView
{
  Q_OBJECT
public:
  ZRegionAnnotationTreeView(ZRegionAnnotationTreeModel &objModel, ZRegionAnnotation &anno, ZDoc &doc, QWidget *parent = nullptr);

protected:
  void contextMenu(const QPoint &pos);

  void indexClicked(const QModelIndex &index);
  void indexDoubleClicked(const QModelIndex &index);
  void indexActivated(const QModelIndex &index);

  void adaptColumns();
  void buttonClickedForUserData(QVariant ud);

  virtual void keyPressEvent(QKeyEvent *e) override;

  void createContextMenu();

private:
  ZRegionAnnotationTreeModel& m_ratModel;
  ZRegionAnnotation& m_regionAnnotation;
  ZDoc &m_doc;
  QSortFilterProxyModel *m_ratProxyModel;
  QMenu *m_contextMenu;
};

} // namespace nim

#endif // ZREGIONANNOTATIONTREEVIEW_H
