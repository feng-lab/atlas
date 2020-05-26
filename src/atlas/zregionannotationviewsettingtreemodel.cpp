#include "zregionannotationviewsettingtreemodel.h"

#include "zobjdoc.h"
#include "zlog.h"
#include "zroifilter.h"
#include "z3dmeshfilter.h"
#include <QLabel>
#include <QApplication>

namespace nim {

ZRegionAnnotationViewSettingTreeModel::ZRegionAnnotationViewSettingTreeModel(
  ZRegionAnnotation& anno,
  std::map<int, std::unique_ptr<ZROIFilter>>& idToROIFilters,
  QObject* parent)
  : ZRegionAnnotationViewSettingTreeModel(anno, parent)
{
  m_idToROIFilters = &idToROIFilters;
}

ZRegionAnnotationViewSettingTreeModel::ZRegionAnnotationViewSettingTreeModel(
  ZRegionAnnotation& anno,
  std::map<int, std::unique_ptr<Z3DMeshFilter>>& idToMeshFilters,
  QObject* parent)
  : ZRegionAnnotationViewSettingTreeModel(anno, parent)
{
  m_idToMeshFilters = &idToMeshFilters;
}

QVariant ZRegionAnnotationViewSettingTreeModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  auto item = static_cast<RegionNode*>(index.internalPointer());

  if (role == Qt::CheckStateRole && index.column() == AbbreviationColumn) {
    auto state = Qt::Unchecked;
    if ((m_idToROIFilters && m_idToROIFilters->at(item->id)->isVisible()) ||
        (m_idToMeshFilters && m_idToMeshFilters->at(item->id)->isVisible())) {
      state = Qt::Checked;
    }
    return state;
  }

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case AbbreviationColumn:
        return QVariant(item->abbreviation);
      case WidgetColumn:
        return "...";
      default:
        break;
    }
  }

  if (role == Qt::UserRole) {
    switch (index.column()) {
      case AbbreviationColumn:
      case WidgetColumn:
        return static_cast<qlonglong>(item->id);
      default:
        break;
    }
  }

  if (role == Qt::ToolTipRole) {
    switch (index.column()) {
      case AbbreviationColumn:
        return QVariant(QString("Region: %1, ID: %2").arg(item->name).arg(item->id));
      case WidgetColumn:
        return QVariant(QString("Modify %1 view setting").arg(item->abbreviation));
      default:
        break;
    }
  }

  return QVariant();
}

Qt::ItemFlags ZRegionAnnotationViewSettingTreeModel::flags(const QModelIndex& index) const
{
  if (!index.isValid())
    return Qt::ItemFlags();

  Qt::ItemFlags flags = Qt::ItemIsEnabled;

  if (index.column() == AbbreviationColumn)
    flags |= Qt::ItemIsUserCheckable;

  return flags;
}

bool ZRegionAnnotationViewSettingTreeModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid())
    return false;

  if (role == Qt::CheckStateRole && index.column() == AbbreviationColumn) {
    auto cs = static_cast<Qt::CheckState>(value.toInt());

    auto item = static_cast<RegionNode*>(index.internalPointer());
    if (m_idToROIFilters) {
      m_idToROIFilters->at(item->id)->setVisible(cs == Qt::Checked);
    } else if (m_idToMeshFilters) {
      m_idToMeshFilters->at(item->id)->setVisible(cs == Qt::Checked);
    }
    emit dataChanged(index, index);
//    // update child items check state
//    updateChildCheckState(index, cs);
//    // update parent items
//    updateParentCheckState(index);

    return true;
  }

  return false;
}

QVariant ZRegionAnnotationViewSettingTreeModel::headerData(int section, Qt::Orientation orientation,
                                                           int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case AbbreviationColumn:
        return QString("Region");
        break;
      case WidgetColumn:
        return QString("View Settings");
        break;
      default:
        break;
    }
  }

  if (orientation == Qt::Horizontal && role == Qt::UserRole) {
    switch (section) {
      case WidgetColumn:
        return 1;
        break;
      default:
        return 0;
        break;
    }
  }

  return QVariant();
}

QModelIndex ZRegionAnnotationViewSettingTreeModel::index(int row, int column, const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent))
    return QModelIndex();

  if (!parent.isValid()) {
    CHECK(row < static_cast<int>(m_annotationTree.numRoots()));
    auto it = m_annotationTree.beginRoot();
    std::advance(it, row);
    RegionNode* node = &(*it);
    return createIndex(row, column, node);
  } else {
    CHECK(row < static_cast<int>(m_annotationTree.numChildren(
      m_nodeToIter.at(static_cast<RegionNode*>(parent.internalPointer())))));
    auto it = m_annotationTree.beginChild(m_nodeToIter.at(static_cast<RegionNode*>(parent.internalPointer())));
    std::advance(it, row);
    RegionNode* node = &(*it);
    return createIndex(row, column, node);
  }
}

QModelIndex ZRegionAnnotationViewSettingTreeModel::parent(const QModelIndex& index) const
{
  if (!index.isValid())
    return QModelIndex();

  auto it = m_nodeToIter.at(static_cast<RegionNode*>(index.internalPointer()));
  if (m_annotationTree.isRoot(it)) {
    return QModelIndex();
  }

  auto pit = m_annotationTree.parent(it);
  RegionNode* node = &*pit;
  if (m_annotationTree.isRoot(pit)) {
    int row = 0;
    for (auto rit = m_annotationTree.beginRoot(); rit != m_annotationTree.endRoot(); ++rit, ++row) {
      if (&*rit == node) {
        return createIndex(row, 0, node);
      }
    }
  } else {
    int row = 0;
    auto ppit = m_annotationTree.parent(pit);
    for (auto rit = m_annotationTree.beginChild(ppit); rit != m_annotationTree.endChild(ppit); ++rit, ++row) {
      if (&*rit == node) {
        return createIndex(row, 0, node);
      }
    }
  }
  return QModelIndex();
}

int ZRegionAnnotationViewSettingTreeModel::rowCount(const QModelIndex& parent) const
{
  if (parent.column() > 0)
    return 0;

  if (!parent.isValid()) {
    return m_annotationTree.numRoots();
  } else {
    auto it = m_nodeToIter.at(static_cast<RegionNode*>(parent.internalPointer()));
    return m_annotationTree.numChildren(it);
  }
}

int ZRegionAnnotationViewSettingTreeModel::columnCount(const QModelIndex& /*parent*/) const
{
  return ColumnCount;
}

void ZRegionAnnotationViewSettingTreeModel::clicked(const QModelIndex& index)
{
  if (index.isValid() && headerData(index.column(), Qt::Horizontal, Qt::UserRole).toInt() == 1) {
    auto item = static_cast<RegionNode*>(index.internalPointer());
    QWidget* wgt = nullptr;
    if (m_idToROIFilters) {
      auto wg = m_idToROIFilters->at(item->id)->viewSettingWidgetsGroupForAnnotationFilter();
      auto label = new QLabel(QString("Region: %1").arg(m_idToROIFilters->at(item->id)->regionName()));
      wgt = wg->createWidget(true, false, label);
    } else if (m_idToMeshFilters) {
      auto wg = m_idToMeshFilters->at(item->id)->widgetsGroupForAnnotationFilter();
      auto label = new QLabel(QString("Region: %1").arg(m_idToMeshFilters->at(item->id)->regionName()));
      wgt = wg->createWidget(true, false, label);
    }
    m_regionViewSettingEditorWindow.reset(wgt);
    if (m_regionViewSettingEditorWindow) {
      m_regionViewSettingEditorWindow->setParent(QApplication::activeWindow());
      m_regionViewSettingEditorWindow->setWindowFlag(Qt::Window, true);
      m_regionViewSettingEditorWindow->showNormal();
      m_regionViewSettingEditorWindow->raise();
      m_regionViewSettingEditorWindow->activateWindow();
    }
  }
  //  if (idxIn.isValid()) {
  //    if (idxIn.column() == ViewSettingColumn) {
  //      ObjItem *item = static_cast<ObjItem*>(idxIn.internalPointer());
  //      if (m_viewSettingCurrentItem == item) {
  //        m_viewSettingCurrentItem = nullptr;
  //        m_regionAnnotation->sendHideViewSettingSignal();
  //      } else {
  //        QModelIndex prevIdx;
  //        for (int row = 0; row < rowCount(); ++row) {
  //          QModelIndex idx = index(row, ViewSettingColumn);
  //          if (static_cast<ObjItem*>(idx.internalPointer()) == m_viewSettingCurrentItem) {
  //            prevIdx = idx;
  //            break;
  //          }
  //          if (rowCount(idx) > 0) {
  //            for (int subRow = 0; subRow < rowCount(idx); ++subRow) {
  //              QModelIndex subIdx = index(subRow, ViewSettingColumn, idx);
  //              if (static_cast<ObjItem*>(subIdx.internalPointer()) == m_viewSettingCurrentItem) {
  //                prevIdx = subIdx;
  //                break;
  //              }
  //            }
  //          }
  //          if (prevIdx.isValid())
  //            break;
  //        }

  //        m_viewSettingCurrentItem = item;
  //        if (prevIdx.isValid())
  //          emit dataChanged(prevIdx, prevIdx);
  //        m_regionAnnotation->sendShowViewSettingSignal(item->id);
  //      }
  //      emit dataChanged(idxIn, idxIn);
  //    }
  //  }
}

void ZRegionAnnotationViewSettingTreeModel::doubleClicked(const QModelIndex& /*unused*/)
{
}

void ZRegionAnnotationViewSettingTreeModel::activated(const QModelIndex& /*idxIn*/)
{
  //  size_t id = indexToId(idxIn);
  //  if (id > 0) {
  //    //LOG(INFO) << id;
  //    m_regionAnnotation->sendOpenEditWidgetSignal(id);
  //  }
}

ZRegionAnnotationViewSettingTreeModel::ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno, QObject* parent)
  : QAbstractItemModel(parent)
  , m_regionAnnotation(anno)
  , m_annotationTree(m_regionAnnotation.annotationTree())
{
  for (auto it = m_annotationTree.begin(); it != m_annotationTree.end(); ++it) {
    m_nodeToIter[&(*it)] = it;
  }
}

} // namespace nim

