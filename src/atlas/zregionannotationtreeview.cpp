#include "zregionannotationtreeview.h"

#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QMenu>
#include "zstyleditemdelegate.h"
#include "QsLog.h"
#include "zbuttoncolumndelegate.h"
#include "zroidoc.h"
#include "zmeshdoc.h"
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

  connect(this, &ZRegionAnnotationTreeView::customContextMenuRequested, this, &ZRegionAnnotationTreeView::contextMenu);
  connect(this, &ZRegionAnnotationTreeView::clicked, this, &ZRegionAnnotationTreeView::indexClicked);
  connect(this, &ZRegionAnnotationTreeView::doubleClicked, this, &ZRegionAnnotationTreeView::indexDoubleClicked);
  connect(this, &ZRegionAnnotationTreeView::activated, this, &ZRegionAnnotationTreeView::indexActivated);

  setMinimumWidth(400);

  setAlternatingRowColors(true);
  QPalette p = palette();
  p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  setPalette(p);

  createContextMenu();

  ZButtonColumnDelegate *delegate = new ZButtonColumnDelegate(this);
  setMouseTracking(true);
  setItemDelegate(delegate);
  connect(this, &ZRegionAnnotationTreeView::entered, delegate, &ZButtonColumnDelegate::cellEntered);
  connect(delegate, &ZButtonColumnDelegate::buttonClickedForUserData, this, &ZRegionAnnotationTreeView::buttonClickedForUserData);

  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsInserted,
          this, &ZRegionAnnotationTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsRemoved,
          this, &ZRegionAnnotationTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::modelReset,
          this, &ZRegionAnnotationTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::layoutChanged,
          this, &ZRegionAnnotationTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::dataChanged,
          this, &ZRegionAnnotationTreeView::adaptColumns);
  adaptColumns();
}

void ZRegionAnnotationTreeView::contextMenu(const QPoint &pos)
{
  Q_UNUSED(pos)
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
  resizeColumnToContents(ZRegionAnnotationTreeModel::MergeROIColumn);
  resizeColumnToContents(ZRegionAnnotationTreeModel::ExportROIColumn);
  resizeColumnToContents(ZRegionAnnotationTreeModel::ExportMeshColumn);
}

void ZRegionAnnotationTreeView::buttonClickedForUserData(QVariant ud)
{
  bool ok;
  int64_t regionID = ud.toLongLong(&ok);
  int64_t action = regionID % 10;
  regionID = regionID / 10;
  assert(ok);
  //LINFO() << regionID;
  if (action == 1) {
    if (!m_doc.roiDoc().hasObj()) {
      QMessageBox::critical(this, tr("Error"), tr("No ROI to merge, try creating some ROI first"));
      return;
    }
    size_t objID = m_doc.roiDoc().chooseOneObjWithWidget(QString("Choose ROI to merge into region %1").arg(regionID), this);
    if (objID)
      m_regionAnnotation.mergeROIToRegion(m_doc.roiDoc().roi(objID), regionID);
  } else if (action == 2) {
    if (m_regionAnnotation.roiOfRegion(regionID)) {
      m_doc.roiDoc().askToSave(*m_regionAnnotation.roiOfRegion(regionID), QString("Export ROI of Region %1").arg(regionID));
    } else {
      QMessageBox::critical(this, tr("Error"), tr("Region %1 is empty and contains no roi").arg(regionID));
      return;
    }
  } else if (action == 3) {
    if (m_regionAnnotation.meshOfRegion(regionID)) {
      m_doc.meshDoc().askToSave(*m_regionAnnotation.meshOfRegion(regionID), QString("Export Mesh of Region %1").arg(regionID));
    } else {
      QMessageBox::critical(this, tr("Error"), tr("Region %1 is empty or contains no mesh").arg(regionID));
      return;
    }
  }
}

void ZRegionAnnotationTreeView::keyPressEvent(QKeyEvent *e)
{
  Q_UNUSED(e)
}

void ZRegionAnnotationTreeView::createContextMenu()
{
}

} // namespace nim

