#include "zregionannotationviewsettingtreeview.h"

#include "zstyleditemdelegate.h"
#include "zregionannotationviewsettingcolumndelegate.h"
#include "zlog.h"
#include "ztheme.h"
#include "zroidoc.h"
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

ZRegionAnnotationViewSettingTreeView::ZRegionAnnotationViewSettingTreeView(
  ZRegionAnnotationViewSettingTreeModel& objModel,
  ZRegionAnnotation& anno,
  std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
  QWidget* parent)
  : QTreeView(parent)
  , m_ratModel(objModel)
  , m_regionAnnotation(anno)
  , m_idToROIFilters(idToROIFilters)
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

  auto delegate = new ZRegionAnnotationViewSettingColumnDelegate(m_idToROIFilters, this);
  setMouseTracking(false);
  setItemDelegate(delegate);
//  connect(this, &ZRegionAnnotationViewSettingTreeView::entered,
//          delegate, &ZRegionAnnotationViewSettingColumnDelegate::cellEntered);
//  connect(delegate, &ZRegionAnnotationViewSettingColumnDelegate::buttonClickedForUserData,
//          this, &ZRegionAnnotationViewSettingTreeView::buttonClickedForUserData);

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

  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  //setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  //verticalScrollBar()->setDisabled(true);
  // setVerticalScrollMode(QTreeView::ScrollPerItem);

  setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  adaptColumns();
  //setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
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

} // namespace nim

