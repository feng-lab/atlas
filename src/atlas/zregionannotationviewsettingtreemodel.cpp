#include "zregionannotationviewsettingtreemodel.h"

#include "zobjdoc.h"
#include "zlog.h"

namespace nim {

ZRegionAnnotationViewSettingTreeModel::ZRegionAnnotationViewSettingTreeModel(ZRegionAnnotation& anno, QObject* parent)
  : QAbstractItemModel(parent)
  , m_regionAnnotation(anno)
  , m_annotationTree(m_regionAnnotation.annotationTree())
{
  for (auto it = m_annotationTree.begin(); it != m_annotationTree.end(); ++it) {
    m_nodeToIter[&(*it)] = it;
  }
}

QVariant ZRegionAnnotationViewSettingTreeModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  auto item = static_cast<RegionNode*>(index.internalPointer());

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case AbbreviationColumn:
        return QVariant(item->abbreviation + QString("_%1").arg(item->id));
      case WidgetColumn:
        return qlonglong(item->id);
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

  return QVariant();
}

Qt::ItemFlags ZRegionAnnotationViewSettingTreeModel::flags(const QModelIndex& index) const
{
  if (!index.isValid())
    return 0;

  Qt::ItemFlags flags = Qt::ItemIsEnabled;

  return flags;
}

bool ZRegionAnnotationViewSettingTreeModel::setData(const QModelIndex& index, const QVariant& /*value*/, int /*role*/)
{
  if (!index.isValid())
    return false;

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
        return QString("");
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

void ZRegionAnnotationViewSettingTreeModel::clicked(const QModelIndex& /*idxIn*/)
{
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

} // namespace nim

