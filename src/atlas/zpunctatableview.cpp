#include "zpunctatableview.h"

#include "zstyleditemdelegate.h"
#include "zlog.h"
#include <QMessageBox>
#include <QApplication>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>

namespace nim {

ZPunctaTableView::ZPunctaTableView(ZPunctaTableModel& objModel, ZPunctaPack& p, ZDoc& doc, QWidget* parent)
  : QTableView(parent)
  , m_ratModel(objModel)
  , m_puncta(p)
  , m_doc(doc)
{
  setSortingEnabled(true);
  m_ratProxyModel = new QSortFilterProxyModel(this);
  m_ratProxyModel->setSourceModel(&m_ratModel);
  m_ratProxyModel->setDynamicSortFilter(true);
  setModel(m_ratProxyModel);
  setContextMenuPolicy(Qt::CustomContextMenu);
  sortByColumn(ZPunctaTableModel::ScoreColumn, Qt::AscendingOrder);

  connect(this, &ZPunctaTableView::customContextMenuRequested, this, &ZPunctaTableView::contextMenu);
  connect(this, &ZPunctaTableView::clicked, this, &ZPunctaTableView::indexClicked);
  connect(this, &ZPunctaTableView::doubleClicked, this, &ZPunctaTableView::indexDoubleClicked);
  connect(this, &ZPunctaTableView::activated, this, &ZPunctaTableView::indexActivated);

  setMinimumWidth(400);

  setSelectionBehavior(QTableView::SelectRows);

  setAlternatingRowColors(true);
  //QPalette p = palette();
  //p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  //setPalette(p);

  createContextMenu();

  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsInserted,
          this, &ZPunctaTableView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsRemoved,
          this, &ZPunctaTableView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::modelReset,
          this, &ZPunctaTableView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::layoutChanged,
          this, &ZPunctaTableView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::dataChanged,
          this, &ZPunctaTableView::adaptColumns);
  adaptColumns();
}

void ZPunctaTableView::contextMenu(const QPoint& /*pos*/)
{
//  if (m_doc->numSelectedObjs() > 0) {
//    m_contextMenu->popup(mapToGlobal(pos));
//  }
}

void ZPunctaTableView::indexClicked(const QModelIndex& index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void ZPunctaTableView::indexDoubleClicked(const QModelIndex& index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));
}

void ZPunctaTableView::indexActivated(const QModelIndex& index)
{
  m_ratModel.activated(m_ratProxyModel->mapToSource(index));
}

void ZPunctaTableView::adaptColumns()
{
  resizeColumnToContents(ZPunctaTableModel::ScoreColumn);
  resizeColumnToContents(ZPunctaTableModel::XColumn);
  resizeColumnToContents(ZPunctaTableModel::YColumn);
  resizeColumnToContents(ZPunctaTableModel::ZColumn);
  resizeColumnToContents(ZPunctaTableModel::RadiusColumn);
  resizeColumnToContents(ZPunctaTableModel::VolSizeColumn);
}

void ZPunctaTableView::keyPressEvent(QKeyEvent* /*e*/)
{
}

void ZPunctaTableView::createContextMenu()
{
}

} // namespace nim

