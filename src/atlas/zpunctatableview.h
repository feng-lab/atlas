#pragma once

#include "zdoc.h"
#include "zpunctatablemodel.h"
#include <QTableView>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class ZPunctaTableView : public QTableView
{
Q_OBJECT
public:
  ZPunctaTableView(ZPunctaTableModel& objModel, ZPuncta& p, ZDoc& doc, QWidget* parent = nullptr);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void keyPressEvent(QKeyEvent* e) override;

  void createContextMenu();

private:
  ZPunctaTableModel& m_ratModel;
  ZPuncta& m_puncta;
  ZDoc& m_doc;
  QSortFilterProxyModel* m_ratProxyModel = nullptr;
  QMenu* m_contextMenu = nullptr;
};

} // namespace nim

