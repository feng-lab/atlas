#include "zpunctatableview.h"

#include "zstyleditemdelegate.h"
#include "zlog.h"
#include <QMessageBox>
#include <QApplication>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QKeySequence>
#include <set>

namespace nim {

ZPunctaTableView::ZPunctaTableView(ZPunctaTableModel& objModel, ZPunctaPack& pun, ZDoc& doc, QWidget* parent)
  : QTableView(parent)
  , m_ratModel(objModel)
  , m_punctaPack(pun)
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

  onPunctaSelectionChanged();

  connect(&m_punctaPack, &ZPunctaPack::selectionChanged, this, &ZPunctaTableView::onPunctaSelectionChanged);
  connect(&m_punctaPack, &ZPunctaPack::punctaChanged, this, &ZPunctaTableView::onPunctaChanged);
  connect(&m_punctaPack, &ZPunctaPack::lockedStateChanged, this, &ZPunctaTableView::onLockedStateChanged);
}

void ZPunctaTableView::contextMenu(const QPoint& pos)
{
  if (m_punctaPack.isLocked()) {
    return;
  }
  if (!m_punctaPack.selectedPuncta().empty()) {
    m_punctaPack.contextMenu().popup(mapToGlobal(pos));
  }
}

void ZPunctaTableView::indexClicked(const QModelIndex& index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void ZPunctaTableView::indexDoubleClicked(const QModelIndex& index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));

  auto row = m_ratProxyModel->mapToSource(index).row();
  auto p = m_punctaPack.punctaPts()[row];
  emit m_doc.requestToAdjustViewToPosition(p->x(), p->y(), p->z(), 128);
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

void ZPunctaTableView::keyPressEvent(QKeyEvent* e)
{
  if (m_punctaPack.isLocked()) {
    return;
  }
  // LOG(INFO) << QKeySequence::listToString(QKeySequence::keyBindings(QKeySequence::Delete));
  switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      m_punctaPack.deleteSelectedPuncta();
      break;
    default:
      QTableView::keyPressEvent(e);
      break;
  }
}

void ZPunctaTableView::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  QTableView::selectionChanged(selected, deselected);

  if (m_punctaPack.isLocked()) {
    return;
  }

  if (m_skipSelectionChangedProcessing) {
    return;
  }

  std::set<int> selectedRows;
  for (const auto& sindex : selectedIndexes()) {
    selectedRows.insert(m_ratProxyModel->mapToSource(sindex).row());
  }
  std::set<const ZPunctum*> selectedPuncta;
  for (auto r : selectedRows) {
    selectedPuncta.insert(m_punctaPack.punctaPts()[r]);
  }
  // LOG(INFO) << selectedPuncta.size();
  m_ignoreSelectionChangedSignal = true;
  m_punctaPack.setSelectedPuncta(selectedPuncta);
  m_ignoreSelectionChangedSignal = false;
}

void ZPunctaTableView::onPunctaSelectionChanged()
{
  if (m_ignoreSelectionChangedSignal || !isVisible()) {
    return;
  }

  // blockSignals(true);
  m_skipSelectionChangedProcessing = true;

  if (m_punctaPack.selectedPuncta().empty()) {
    selectionModel()->clearSelection();
  } else {
    bool first = true;
    for (auto p : m_punctaPack.selectedPuncta()) {
      auto index = m_ratProxyModel->mapFromSource(m_ratModel.index(m_punctaPack.punctumToRow().at(p), 0));
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

void ZPunctaTableView::onPunctaChanged()
{
  m_ratModel.updateModel();

  onPunctaSelectionChanged();
}

void ZPunctaTableView::onLockedStateChanged(bool)
{
}

} // namespace nim

