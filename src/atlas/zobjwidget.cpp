#include "zobjwidget.h"

#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QMenu>
#include "zdoc.h"
#include "zshareitemselectionmodel.h"
#include "zobjmodel.h"
#include "zstyleditemdelegate.h"
#include "QsLog.h"

namespace nim {

ZObjWidget::ZObjWidget(ZDoc *doc, ZObjModel *objModel, QItemSelectionModel *selectionModel, QWidget *parent)
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
  sortByColumn(ZObjModel::TypeColumn);
  setStyleSheet(
        "QTreeView::indicator:unchecked {image: url(:/icons/eye_close-512.png);}"
        "QTreeView::indicator:checked {image: url(:/icons/eye_open-512.png);}"
        "QTreeView::indicator:indeterminate {image: url(:/icons/eye_half-512.png);}"
        );

  connect(this, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextMenu(QPoint)));
  connect(this, SIGNAL(clicked(QModelIndex)), this, SLOT(indexClicked(QModelIndex)));
  connect(this, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(indexDoubleClicked(QModelIndex)));
  connect(this, SIGNAL(activated(QModelIndex)), this, SLOT(indexActivated(QModelIndex)));

  setMinimumWidth(400);

  setAlternatingRowColors(true);
  QPalette p = palette();
  p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  setPalette(p);

  createContextMenu();

#if 0
  ZStyledItemDelegate *delegate = new ZStyledItemDelegate(this);
  setMouseTracking(true);
  setItemDelegate(delegate);
  connect(this, SIGNAL(entered(QModelIndex)), delegate, SLOT(cellEntered(QModelIndex)));
#endif

  connect(m_objProxyModel, SIGNAL(rowsInserted(QModelIndex,int,int)),
          this, SLOT(adaptColumns()));
  connect(m_objProxyModel, SIGNAL(rowsRemoved(QModelIndex,int,int)),
          this, SLOT(adaptColumns()));
  connect(m_objProxyModel, SIGNAL(modelReset()),
          this, SLOT(adaptColumns()));
  connect(m_objProxyModel, SIGNAL(layoutChanged()),
          this, SLOT(adaptColumns()));
  connect(m_objProxyModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
          this, SLOT(adaptColumns()));
  adaptColumns();
}

void ZObjWidget::contextMenu(const QPoint &pos)
{
  if (m_doc->numSelectedObjs() > 0) {
    m_contextMenu->popup(mapToGlobal(pos));
  }
}

void ZObjWidget::indexClicked(const QModelIndex &index)
{
  m_objModel->clicked(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::indexDoubleClicked(const QModelIndex &index)
{
  m_objModel->doubleClicked(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::indexActivated(const QModelIndex &index)
{
  m_objModel->activated(m_objProxyModel->mapToSource(index));
}

void ZObjWidget::adaptColumns()
{
  //header()->resizeSection(0, 20);
  resizeColumnToContents(ZObjModel::ShowHideNameColumn);
  resizeColumnToContents(ZObjModel::NameColumn);
  resizeColumnToContents(ZObjModel::TypeColumn);
  resizeColumnToContents(ZObjModel::ShowHideColumn);
  resizeColumnToContents(ZObjModel::LockColumn);
}

void ZObjWidget::keyPressEvent(QKeyEvent *e)
{
  switch(e->key()) {
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
  m_contextMenu->addAction(QString("Show"), m_doc, SLOT(showSelectedObjs()));
  m_contextMenu->addAction(QString("Hide"), m_doc, SLOT(hideSelectedObjs()));
  m_contextMenu->addSeparator();
  m_contextMenu->addAction(QString("Make Alias"), m_doc, SLOT(makeAliasOfSelectedObjs()));
  m_contextMenu->addAction(QString("Remove"), m_doc, SLOT(removeSelectedObjs()));
  m_contextMenu->addSeparator();
  m_contextMenu->addAction(QString("Save"), m_doc, SLOT(saveSelectedObjs()));
  m_contextMenu->addAction(QString("Save As..."), m_doc, SLOT(saveSelectedObjsAs()));
  m_contextMenu->addSeparator();
#ifdef Q_OS_MAC
  m_contextMenu->addAction(QString("Show in Finder"), m_doc, SLOT(showSelectedObjsInGraphicalShell()));
#endif
#ifdef Q_OS_WIN
  m_contextMenu->addAction(QString("Show in Explorer"), m_doc, SLOT(showSelectedObjsInGraphicalShell()));
#endif
  m_contextMenu->addAction(QString("Copy Full Path"), m_doc, SLOT(copySelectedObjsPathToClipboard()));
}

} // namespace nim
