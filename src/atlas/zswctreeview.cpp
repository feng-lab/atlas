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
  sortByColumn(ZSwcTreeModel::IDColumn, Qt::AscendingOrder);

  connect(this, &ZSwcTreeView::customContextMenuRequested, this, &ZSwcTreeView::contextMenu);
  connect(this, &ZSwcTreeView::clicked, this, &ZSwcTreeView::indexClicked);
  connect(this, &ZSwcTreeView::doubleClicked, this, &ZSwcTreeView::indexDoubleClicked);
  connect(this, &ZSwcTreeView::activated, this, &ZSwcTreeView::indexActivated);

  setAlternatingRowColors(true);
  //QPalette p = palette();
  //p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  //setPalette(p);

  setSelectionMode(QTreeView::ExtendedSelection);

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

  onSwcSelectionChanged();

  connect(&m_swcPack, &ZSwcPack::selectionChanged, this, &ZSwcTreeView::onSwcSelectionChanged);
  connect(&m_swcPack, &ZSwcPack::swcChanged, this, &ZSwcTreeView::onSwcChanged);
  connect(&m_swcPack, &ZSwcPack::lockedStateChanged, this, &ZSwcTreeView::onLockedStateChanged);
}

void ZSwcTreeView::contextMenu(const QPoint& pos)
{
  if (m_swcPack.isLocked()) {
    return;
  }
  if (!m_swcPack.selectedNodes().empty()) {
    m_swcPack.contextMenu().popup(mapToGlobal(pos));
  }
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

void ZSwcTreeView::keyPressEvent(QKeyEvent* e)
{
  if (m_swcPack.isLocked()) {
    return;
  }
  // LOG(INFO) << QKeySequence::listToString(QKeySequence::keyBindings(QKeySequence::Delete));
  switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      m_swcPack.deleteSelectedNodes();
      break;
    default:
      QTreeView::keyPressEvent(e);
      break;
  }
}

void ZSwcTreeView::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
  QTreeView::selectionChanged(selected, deselected);

  if (m_swcPack.isLocked()) {
    return;
  }

  if (m_skipSelectionChangedProcessing) {
    return;
  }

  std::set<std::tuple<int, int>> selectedRows;
  for (const auto& sindex : selectedIndexes()) {
    auto index = m_ratProxyModel->mapToSource(sindex);
    if (!index.parent().isValid()) {
      selectedRows.insert(std::make_tuple(index.row(), -1));
    } else {
      selectedRows.insert(std::make_tuple(index.parent().row(), index.row()));
    }
  }
  std::set<ZSwc::SwcTreeNode> selectedNodes;
  for (const auto& r : selectedRows) {
    selectedNodes.insert(m_swcPack.getNodeOfParentRowAndRow(r));
  }
  // LOG(INFO) << selectedNodes.size();
  m_ignoreSelectionChangedSignal = true;
  m_swcPack.setSelectedNodes(selectedNodes);
  m_ignoreSelectionChangedSignal = false;
}

void ZSwcTreeView::onSwcSelectionChanged()
{
  if (m_ignoreSelectionChangedSignal) {
    return;
  }

  // blockSignals(true);
  m_skipSelectionChangedProcessing = true;

  if (m_swcPack.selectedNodes().empty()) {
    selectionModel()->clearSelection();
  } else {
    bool first = true;
    for (auto p : m_swcPack.selectedNodes()) {
      auto [parentRow, row] = m_swcPack.getParentRowAndRowOfNode(p);
      auto index = m_ratProxyModel->mapFromSource(row >= 0 ?
        m_ratModel.index(row, 0, m_ratModel.index(parentRow, 0, QModelIndex())) :
        m_ratModel.index(parentRow, 0, QModelIndex()));
      if (first) {
        selectionModel()->select(index, QItemSelectionModel::Rows | QItemSelectionModel::ClearAndSelect);
        scrollTo(index);
        first = false;
      } else {
        selectionModel()->select(index, QItemSelectionModel::Rows | QItemSelectionModel::Select);
      }
    }
  }

  m_skipSelectionChangedProcessing = false;
  // blockSignals(false);
}

void ZSwcTreeView::onSwcChanged()
{
  m_ratModel.updateModel();

  onSwcSelectionChanged();
}

void ZSwcTreeView::onLockedStateChanged(bool)
{
}

} // namespace nim

