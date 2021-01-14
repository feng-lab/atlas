#include "zregionannotationviewsettingtreeview.h"

#include "zstyleditemdelegate.h"
#include "zregionannotationviewsettingcolumndelegate.h"
#include "ztheme.h"
#include "zroidoc.h"
#include "zmeshdoc.h"
#include "zroifilter.h"
#include "z3dmeshfilter.h"
#include <QMessageBox>
#include <QScrollBar>
#include <QLabel>
#include <QSortFilterProxyModel>
#include <QKeyEvent>
#include <QHeaderView>

namespace nim {

ZRegionAnnotationViewSettingTreeView::ZRegionAnnotationViewSettingTreeView(
  ZRegionAnnotationViewSettingTreeModel& objModel,
  ZRegionAnnotation& anno,
  std::map<int64_t, std::unique_ptr<ZROIFilter>>& idToROIFilters,
  QWidget* parent)
  : ZRegionAnnotationViewSettingTreeView(objModel, anno, parent)
{
  auto delegate = new ZRegionAnnotationViewSettingColumnDelegate(idToROIFilters, this);
  setMouseTracking(false);
  setItemDelegate(delegate);
}

ZRegionAnnotationViewSettingTreeView::ZRegionAnnotationViewSettingTreeView(
  ZRegionAnnotationViewSettingTreeModel& objModel,
  ZRegionAnnotation& anno,
  std::map<int64_t, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
  QWidget* parent)
  : ZRegionAnnotationViewSettingTreeView(objModel, anno, parent)
{
  auto delegate = new ZRegionAnnotationViewSettingColumnDelegate(idToMeshFilters, this);
  setMouseTracking(false);
  setItemDelegate(delegate);
}

void ZRegionAnnotationViewSettingTreeView::contextMenu(const QPoint& /*pos*/)
{
//  if (m_doc->numSelectedObjs() > 0) {
//    m_contextMenu->popup(mapToGlobal(pos));
//  }
}

void ZRegionAnnotationViewSettingTreeView::indexClicked(const QModelIndex& index)
{
  m_ratModel.clicked(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationViewSettingTreeView::indexDoubleClicked(const QModelIndex& index)
{
  m_ratModel.doubleClicked(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationViewSettingTreeView::indexActivated(const QModelIndex& index)
{
  m_ratModel.activated(m_ratProxyModel->mapToSource(index));
}

void ZRegionAnnotationViewSettingTreeView::adaptColumns()
{
  resizeColumnToContents(ZRegionAnnotationViewSettingTreeModel::AbbreviationColumn);
  resizeColumnToContents(ZRegionAnnotationViewSettingTreeModel::WidgetColumn);
}

void ZRegionAnnotationViewSettingTreeView::keyPressEvent(QKeyEvent* /*e*/)
{
}

void ZRegionAnnotationViewSettingTreeView::createContextMenu()
{
}

void ZRegionAnnotationViewSettingTreeView::wheelEvent(QWheelEvent* e)
{
  e->ignore();
}

ZRegionAnnotationViewSettingTreeView::ZRegionAnnotationViewSettingTreeView(
  ZRegionAnnotationViewSettingTreeModel &objModel,
  ZRegionAnnotation &anno,
  QWidget* parent)
  : QTreeView(parent)
  , m_ratModel(objModel)
  , m_regionAnnotation(anno)
{
  setSortingEnabled(true);
  setExpandsOnDoubleClick(false);
  m_ratProxyModel = new QSortFilterProxyModel(this);
  m_ratProxyModel->setSourceModel(&m_ratModel);
  m_ratProxyModel->setDynamicSortFilter(true);
  setModel(m_ratProxyModel);
  setContextMenuPolicy(Qt::CustomContextMenu);
  sortByColumn(ZRegionAnnotationViewSettingTreeModel::AbbreviationColumn, Qt::AscendingOrder);
  setStyleSheet(
    QString("QTreeView::indicator:unchecked {image: url(%1);}"
            "QTreeView::indicator:checked {image: url(%2);}"
            "QTreeView::indicator:indeterminate {image: url(%3);}")
      .arg(ZTheme::instance().iconFile(ZTheme::EyeCloseIcon))
      .arg(ZTheme::instance().iconFile(ZTheme::EyeOpenIcon))
      .arg(ZTheme::instance().iconFile(ZTheme::EyeHalfIcon))
  );

  connect(this, &ZRegionAnnotationViewSettingTreeView::customContextMenuRequested,
          this, &ZRegionAnnotationViewSettingTreeView::contextMenu);
  connect(this, &ZRegionAnnotationViewSettingTreeView::clicked,
          this, &ZRegionAnnotationViewSettingTreeView::indexClicked);
  connect(this, &ZRegionAnnotationViewSettingTreeView::doubleClicked,
          this, &ZRegionAnnotationViewSettingTreeView::indexDoubleClicked);
  connect(this, &ZRegionAnnotationViewSettingTreeView::activated,
          this, &ZRegionAnnotationViewSettingTreeView::indexActivated);

  setMinimumWidth(200);

  setAlternatingRowColors(true);
  //QPalette p = palette();
  //p.setColor(QPalette::AlternateBase, QColor(240, 240, 240));
  //setPalette(p);

  createContextMenu();

  connect(this, &ZRegionAnnotationViewSettingTreeView::expanded,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);

  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsInserted,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::rowsRemoved,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::modelReset,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::layoutChanged,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);
  connect(m_ratProxyModel, &QSortFilterProxyModel::dataChanged,
          this, &ZRegionAnnotationViewSettingTreeView::adaptColumns);

  //setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  verticalScrollBar()->setDisabled(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  horizontalScrollBar()->setDisabled(true);
  // setVerticalScrollMode(QTreeView::ScrollPerItem);

  setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  adaptColumns();
  header()->setStretchLastSection(false);
}

} // namespace nim

