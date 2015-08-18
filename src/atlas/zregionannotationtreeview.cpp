#include "zregionannotationtreeview.h"

#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QMenu>
#include "zstyleditemdelegate.h"
#include "QsLog.h"
#include "zbuttoncolumndelegate.h"
#include "zroidoc.h"
#include <QMessageBox>

namespace nim {

ZRegionAnnotationTreeView::ZRegionAnnotationTreeView(ZRegionAnnotationTreeModel &objModel, ZRegionAnnotation &anno, ZDoc &doc, QWidget *parent)
  : QTreeView(parent)
  , m_ratModel(objModel)
  , m_regionAnnotation(anno)
  , m_doc(doc)
{
  setSortingEnabled(true);
  setExpandsOnDoubleClick(false);
  m_ratProxyModel = new QSortFilterProxyModel(this);
  m_ratProxyModel->setSourceModel(&m_ratModel);
  m_ratProxyModel->setDynamicSortFilter(true);
  setModel(m_ratProxyModel);
  setContextMenuPolicy(Qt::CustomContextMenu);
  sortByColumn(ZRegionAnnotationTreeModel::AbbreviationColumn);

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

  ZButtonColumnDelegate *delegate = new ZButtonColumnDelegate(this);
  setMouseTracking(true);
  setItemDelegate(delegate);
  connect(this, SIGNAL(entered(QModelIndex)), delegate, SLOT(cellEntered(QModelIndex)));
  connect(delegate, SIGNAL(buttonClickedForUserData(QVariant)), this, SLOT(buttonClickedForUserData(QVariant)));

  connect(m_ratProxyModel, SIGNAL(rowsInserted(QModelIndex,int,int)),
          this, SLOT(adaptColumns()));
  connect(m_ratProxyModel, SIGNAL(rowsRemoved(QModelIndex,int,int)),
          this, SLOT(adaptColumns()));
  connect(m_ratProxyModel, SIGNAL(modelReset()),
          this, SLOT(adaptColumns()));
  connect(m_ratProxyModel, SIGNAL(layoutChanged()),
          this, SLOT(adaptColumns()));
  connect(m_ratProxyModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
          this, SLOT(adaptColumns()));
  adaptColumns();
}

void ZRegionAnnotationTreeView::contextMenu(const QPoint &pos)
{
//  if (m_doc->numSelectedObjs() > 0) {
//    m_contextMenu->popup(mapToGlobal(pos));
//  }
}

void ZRegionAnnotationTreeView::indexClicked(const QModelIndex &index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationTreeView::indexDoubleClicked(const QModelIndex &index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationTreeView::indexActivated(const QModelIndex &index)
{
  m_ratModel.activated(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationTreeView::adaptColumns()
{
  resizeColumnToContents(ZRegionAnnotationTreeModel::AbbreviationColumn);
  resizeColumnToContents(ZRegionAnnotationTreeModel::NameColumn);
  resizeColumnToContents(ZRegionAnnotationTreeModel::IDColumn);
}

void ZRegionAnnotationTreeView::buttonClickedForUserData(QVariant ud)
{
  if (!m_doc.roiDoc().hasObj()) {
    QMessageBox::critical(this, tr("Error"), tr("No ROI to merge, try creating some ROI first"));
    return;
  }
  bool ok;
  int64_t regionID = ud.toLongLong(&ok);
  assert(ok);
  //LINFO() << regionID;
  size_t objID = m_doc.roiDoc().chooseOneObjWithWidget(QString("Choose ROI to merge into region %1").arg(regionID), this);
  if (objID)
    m_regionAnnotation.mergeROIToRegion(m_doc.roiDoc().roi(objID), regionID);
}

void ZRegionAnnotationTreeView::keyPressEvent(QKeyEvent *e)
{
}

void ZRegionAnnotationTreeView::createContextMenu()
{
}

} // namespace nim

