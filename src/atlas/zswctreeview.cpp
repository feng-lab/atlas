#include "zswctreeview.h"

#include "zstyleditemdelegate.h"
#include "zlog.h"
#include <QMessageBox>
#include <QApplication>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>

namespace nim {

ZSwcTreeView::ZSwcTreeView(ZSwcTreeModel& objModel, ZSwcPack& swcPack,
                           ZDoc& doc, QWidget* parent)
  : QTreeView(parent)
  , m_ratModel(objModel)
  , m_swcPack(swcPack)
  , m_doc(doc)
{
  setSortingEnabled(true);
  setExpandsOnDoubleClick(false);
  m_ratProxyModel = new QSortFilterProxyModel(this);
  m_ratProxyModel->setSourceModel(&m_ratModel);
  m_ratProxyModel->setDynamicSortFilter(true);
  setModel(m_ratProxyModel);
  setContextMenuPolicy(Qt::CustomContextMenu);
  // sortByColumn(ZSwcTreeModel::IDColumn, Qt::AscendingOrder);

  connect(this, &ZSwcTreeView::customContextMenuRequested, this, &ZSwcTreeView::contextMenu);
  connect(this, &ZSwcTreeView::clicked, this, &ZSwcTreeView::indexClicked);
  connect(this, &ZSwcTreeView::doubleClicked, this, &ZSwcTreeView::indexDoubleClicked);
  connect(this, &ZSwcTreeView::activated, this, &ZSwcTreeView::indexActivated);

  setAlternatingRowColors(true);
  //QPalette p = palette();
  //p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  //setPalette(p);

  connect(this, &ZSwcTreeView::expanded,
          this, &ZSwcTreeView::adaptColumns);

  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsInserted,
          this, &ZSwcTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsRemoved,
          this, &ZSwcTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::modelReset,
          this, &ZSwcTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::layoutChanged,
          this, &ZSwcTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::dataChanged,
          this, &ZSwcTreeView::adaptColumns);
  adaptColumns();

  header()->setStretchLastSection(false);
}

void ZSwcTreeView::contextMenu(const QPoint& /*pos*/)
{
//  if (m_doc->numSelectedObjs() > 0) {
//    m_contextMenu->popup(mapToGlobal(pos));
//  }
}

void ZSwcTreeView::indexClicked(const QModelIndex& index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void ZSwcTreeView::indexDoubleClicked(const QModelIndex& index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));
}

void ZSwcTreeView::indexActivated(const QModelIndex& index)
{
  m_ratModel.activated(m_ratProxyModel->mapToSource(index));
}

void ZSwcTreeView::adaptColumns()
{
  resizeColumnToContents(ZSwcTreeModel::IDColumn);
  resizeColumnToContents(ZSwcTreeModel::TypeColumn);
  resizeColumnToContents(ZSwcTreeModel::XColumn);
  resizeColumnToContents(ZSwcTreeModel::YColumn);
  resizeColumnToContents(ZSwcTreeModel::ZColumn);
  resizeColumnToContents(ZSwcTreeModel::RadiusColumn);
  resizeColumnToContents(ZSwcTreeModel::TopologyColumn);
  resizeColumnToContents(ZSwcTreeModel::ParentIDColumn);
  resizeColumnToContents(ZSwcTreeModel::LabelColumn);
}

void ZSwcTreeView::keyPressEvent(QKeyEvent* /*e*/)
{
}

} // namespace nim

