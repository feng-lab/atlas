#pragma once

#include "zdoc.h"
#include "zpunctatablemodel.h"
#include <QTableView>
#include <map>

class QSortFilterProxyModel;

class QMenu;

namespace nim {

class ZPunctaTableView : public QTableView
{
  Q_OBJECT

public:
  ZPunctaTableView(ZPunctaTableModel& objModel, ZPunctaPack& p, ZDoc& doc, QWidget* parent = nullptr);

protected:
  void contextMenu(const QPoint& pos);

  void indexClicked(const QModelIndex& index);

  void indexDoubleClicked(const QModelIndex& index);

  void indexActivated(const QModelIndex& index);

  void adaptColumns();

  void keyPressEvent(QKeyEvent* e) override;

  void selectionChanged(const QItemSelection& selected, const QItemSelection& deselected) override;

  void onPunctaSelectionChanged();

  void onPunctaChanged();

  void onLockedStateChanged(bool l);

private:
  ZPunctaTableModel& m_ratModel;
  ZPunctaPack& m_punctaPack;
  ZDoc& m_doc;
  QSortFilterProxyModel* m_ratProxyModel = nullptr;
  bool m_ignoreSelectionChangedSignal = false;
  bool m_skipSelectionChangedProcessing = false;
};

} // namespace nim
