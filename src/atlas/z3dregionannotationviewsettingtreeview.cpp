#include "z3dregionannotationviewsettingtreeview.h"

#include "zstyleditemdelegate.h"
#include "z3dregionannotationviewsettingcolumndelegate.h"
#include "zlog.h"
#include "ztheme.h"
#include "zmeshdoc.h"
#include <QMessageBox>
#include <QScrollBar>
#include <QLabel>
#include <QApplication>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>
#include <QApplication>

namespace nim {

Z3DRegionAnnotationViewSettingTreeView::Z3DRegionAnnotationViewSettingTreeView(
  Z3DRegionAnnotationViewSettingTreeModel& objModel,
  ZRegionAnnotation& anno,
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
  QWidget* parent)
  : QTreeView(parent)
  , m_ratModel(objModel)
  , m_regionAnnotation(anno)
  , m_idToMeshFilters(idToMeshFilters)
{
  setSortingEnabled(true);
  setExpandsOnDoubleClick(false);
  m_ratProxyModel = new QSortFilterProxyModel(this);
  m_ratProxyModel->setSourceModel(&m_ratModel);
  m_ratProxyModel->setDynamicSortFilter(true);
  setModel(m_ratProxyModel);
  setContextMenuPolicy(Qt::CustomContextMenu);
  sortByColumn(Z3DRegionAnnotationViewSettingTreeModel::AbbreviationColumn, Qt::AscendingOrder);
  setStyleSheet(
    QString("QTreeView::indicator:unchecked {image: url(%1);}"
            "QTreeView::indicator:checked {image: url(%2);}"
            "QTreeView::indicator:indeterminate {image: url(%3);}")
      .arg(ZTheme::instance().iconFile(ZTheme::EyeCloseIcon))
      .arg(ZTheme::instance().iconFile(ZTheme::EyeOpenIcon))
      .arg(ZTheme::instance().iconFile(ZTheme::EyeHalfIcon))
  );

  connect(this, &Z3DRegionAnnotationViewSettingTreeView::customContextMenuRequested,
          this, &Z3DRegionAnnotationViewSettingTreeView::contextMenu);
  connect(this, &Z3DRegionAnnotationViewSettingTreeView::clicked,
          this, &Z3DRegionAnnotationViewSettingTreeView::indexClicked);
  connect(this, &Z3DRegionAnnotationViewSettingTreeView::doubleClicked,
          this, &Z3DRegionAnnotationViewSettingTreeView::indexDoubleClicked);
  connect(this, &Z3DRegionAnnotationViewSettingTreeView::activated,
          this, &Z3DRegionAnnotationViewSettingTreeView::indexActivated);

  setMinimumWidth(200);

  setAlternatingRowColors(true);
  //QPalette p = palette();
  //p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  //setPalette(p);

  createContextMenu();

  auto delegate = new Z3DRegionAnnotationViewSettingColumnDelegate(m_idToMeshFilters, this);
  setMouseTracking(false);
  setItemDelegate(delegate);
//  connect(this, &ZRegionAnnotationViewSettingTreeView::entered,
//          delegate, &ZRegionAnnotationViewSettingColumnDelegate::cellEntered);
//  connect(delegate, &ZRegionAnnotationViewSettingColumnDelegate::buttonClickedForUserData,
//          this, &ZRegionAnnotationViewSettingTreeView::buttonClickedForUserData);

  connect(this, &Z3DRegionAnnotationViewSettingTreeView::expanded,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);

  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsInserted,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsRemoved,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::modelReset,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::layoutChanged,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::dataChanged,
          this, &Z3DRegionAnnotationViewSettingTreeView::adaptColumns);

  //setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  verticalScrollBar()->setDisabled(true);
  // setVerticalScrollMode(QTreeView::ScrollPerItem);

  setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  adaptColumns();
}

void Z3DRegionAnnotationViewSettingTreeView::contextMenu(const QPoint& /*pos*/)
{
//  if (m_doc->numSelectedObjs() > 0) {
//    m_contextMenu->popup(mapToGlobal(pos));
//  }
}

void Z3DRegionAnnotationViewSettingTreeView::indexClicked(const QModelIndex& index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void Z3DRegionAnnotationViewSettingTreeView::indexDoubleClicked(const QModelIndex& index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));
}

void Z3DRegionAnnotationViewSettingTreeView::indexActivated(const QModelIndex& index)
{
  m_ratModel.activated(m_ratProxyModel->mapToSource(index));
}

void Z3DRegionAnnotationViewSettingTreeView::adaptColumns()
{
  resizeColumnToContents(Z3DRegionAnnotationViewSettingTreeModel::AbbreviationColumn);
}

void Z3DRegionAnnotationViewSettingTreeView::keyPressEvent(QKeyEvent* /*e*/)
{
}

void Z3DRegionAnnotationViewSettingTreeView::createContextMenu()
{
}

void Z3DRegionAnnotationViewSettingTreeView::wheelEvent(QWheelEvent* e)
{
  e->ignore();
}

} // namespace nim

