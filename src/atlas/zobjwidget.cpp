#include "zobjwidget.h"

#include "zdoc.h"
#include "zshareitemselectionmodel.h"
#include "zobjmodel.h"
#include "zstyleditemdelegate.h"
#include "zlog.h"
#include "ztheme.h"
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QMenu>

namespace nim {

ZObjWidget::ZObjWidget(ZDoc* doc, ZObjModel* objModel, QItemSelectionModel* selectionModel, QWidget* parent)
  : QTreeView(parent)
  , m_doc(doc)
  , m_objModel(objModel)
{
  setSortingEnabled(true);
  setExpandsOnDoubleClick(false);
  m_objProxyModel = new QSortFilterProxyModel(this);
  m_objProxyModel->setSourceModel(m_objModel);
  m_objProxyModel->setDynamicSortFilter(true);
  setModel(m_objProxyModel);
  setSelectionModel(new ZShareItemSelectionModel(m_objProxyModel, selectionModel, this));
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setSelectionBehavior(QAbstractItemView::SelectItems);
  setContextMenuPolicy(Qt::CustomContextMenu);
  sortByColumn(ZObjModel::TypeColumn, Qt::AscendingOrder);
  setStyleSheet(ZTheme::instance().treeViewIndicatorStyleSheet());
  connect(&ZTheme::instance(), &ZTheme::themeChanged, this, [this]() {
    setStyleSheet(ZTheme::instance().treeViewIndicatorStyleSheet());
    viewport()->update();
  });

  connect(this, &ZObjWidget::customContextMenuRequested, this, &ZObjWidget::contextMenu);
  connect(this, &ZObjWidget::clicked, this, &ZObjWidget::indexClicked);
  connect(this, &ZObjWidget::doubleClicked, this, &ZObjWidget::indexDoubleClicked);
  connect(this, &ZObjWidget::activated, this, &ZObjWidget::indexActivated);

  setMinimumWidth(400);
  setMinimumHeight(250);

  setAlternatingRowColors(true);
  // QPalette p = palette();
  // p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  // setPalette(p);

  createContextMenu();

  m_adaptColumnsTimer.setSingleShot(true);
  m_adaptColumnsTimer.setInterval(0);
  connect(&m_adaptColumnsTimer, &QTimer::timeout, this, &ZObjWidget::adaptColumnsIfPending);

#if 0
  ZStyledItemDelegate *delegate = new ZStyledItemDelegate(this);
  setMouseTracking(true);
  setItemDelegate(delegate);
  connect(this, &ZObjWidget::entered, delegate, &ZStyledItemDelegate::cellEntered);
#endif

  connect(m_objProxyModel, &QSortFilterProxyModel::rowsInserted, this, &ZObjWidget::scheduleAdaptColumns);
  connect(m_objProxyModel, &QSortFilterProxyModel::rowsRemoved, this, &ZObjWidget::scheduleAdaptColumns);
  connect(m_objProxyModel, &QSortFilterProxyModel::modelReset, this, &ZObjWidget::scheduleAdaptColumns);
  connect(m_objProxyModel, &QSortFilterProxyModel::layoutChanged, this, &ZObjWidget::scheduleAdaptColumns);
  connect(m_objProxyModel,
          &QSortFilterProxyModel::dataChanged,
          this,
          [this](const QModelIndex&, const QModelIndex&, const QVector<int>& roles) {
            // Column auto-sizing is O(numRows) and is not needed for show/hide toggles.
            // Only re-size when text changes (or if the sender didn't provide roles).
            if (roles.isEmpty() || roles.contains(Qt::DisplayRole)) {
              scheduleAdaptColumns();
            }
          });
  scheduleAdaptColumns();
}

void ZObjWidget::showEvent(QShowEvent* event)
{
  QTreeView::showEvent(event);
  if (m_adaptColumnsPending) {
    scheduleAdaptColumns();
  }
}

void ZObjWidget::scheduleAdaptColumns()
{
  m_adaptColumnsPending = true;

  if (!isVisible()) {
    return;
  }

  m_adaptColumnsTimer.start();
}

void ZObjWidget::adaptColumnsIfPending()
{
  if (!m_adaptColumnsPending || !isVisible()) {
    return;
  }

  m_adaptColumnsPending = false;
  adaptColumns();
}

void ZObjWidget::contextMenu(const QPoint& pos)
{
  if (m_doc->numSelectedObjs() > 0) {
    m_contextMenu->popup(mapToGlobal(pos));
  }
}

void ZObjWidget::indexClicked(const QModelIndex& index)
{
  m_objModel->clicked(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::indexDoubleClicked(const QModelIndex& index)
{
  m_objModel->doubleClicked(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::indexActivated(const QModelIndex& index)
{
  m_objModel->activated(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::adaptColumns()
{
  // header()->resizeSection(0, 20);
  resizeColumnToContents(ZObjModel::ShowHideNameColumn);
  resizeColumnToContents(ZObjModel::LockColumn);
  // resizeColumnToContents(ZObjModel::NameColumn);
  resizeColumnToContents(ZObjModel::TypeColumn);
  // resizeColumnToContents(ZObjModel::ShowHideColumn);
}

void ZObjWidget::keyPressEvent(QKeyEvent* e)
{
  switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      m_doc->removeSelectedObjs();
      break;
    default:
      QTreeView::keyPressEvent(e);
      break;
  }
}

void ZObjWidget::createContextMenu()
{
  m_contextMenu = new QMenu(this);
  m_contextMenu->addAction(QString("Show"), m_doc, &ZDoc::showSelectedObjs);
  m_contextMenu->addAction(QString("Hide"), m_doc, &ZDoc::hideSelectedObjs);
  m_contextMenu->addSeparator();
  m_contextMenu->addAction(QString("Lock"), m_doc, &ZDoc::lockSelectedObjs);
  m_contextMenu->addAction(QString("Unlock"), m_doc, &ZDoc::unlockSelectedObjs);
  m_contextMenu->addSeparator();
  m_contextMenu->addAction(QString("Make Alias"), m_doc, &ZDoc::makeAliasOfSelectedObjs);
  m_contextMenu->addAction(QString("Remove"), m_doc, &ZDoc::removeSelectedObjs);
  m_contextMenu->addSeparator();
  m_contextMenu->addAction(QString("Save"), m_doc, &ZDoc::saveSelectedObjs);
  m_contextMenu->addAction(QString("Save As..."), m_doc, &ZDoc::saveSelectedObjsAs);
  m_contextMenu->addSeparator();
#ifdef Q_OS_MAC
  m_contextMenu->addAction(QString("Show in Finder"), m_doc, &ZDoc::showSelectedObjsInGraphicalShell);
#elif defined(Q_OS_WIN)
  m_contextMenu->addAction(QString("Show in Explorer"), m_doc, &ZDoc::showSelectedObjsInGraphicalShell);
#else
  m_contextMenu->addAction(QString("Show in Files"), m_doc, &ZDoc::showSelectedObjsInGraphicalShell);
#endif
  m_contextMenu->addAction(QString("Copy Full Path"), m_doc, &ZDoc::copySelectedObjsPathToClipboard);
}

} // namespace nim
