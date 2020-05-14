#include "zswctreemodel.h"

#include "zobjdoc.h"
#include "zlog.h"

namespace nim {

ZSwcTreeModel::ZSwcTreeModel(ZSwcPack& swcPack, ZDoc& doc, QObject* parent)
  : QAbstractItemModel(parent)
  , m_swcPack(swcPack)
  , m_doc(doc)
{
  m_roots.clear();
  m_rootToChildren.clear();
  for (auto rit = m_swcPack.swc().beginRoot(); rit != m_swcPack.swc().endRoot(); ++rit) {
    m_roots.emplace_back(rit);
  }
  for (auto& rit : m_roots) {
    m_rootToChildren[&rit].clear();
    auto it = m_swcPack.swc().begin(rit);
    CHECK(it == rit);
    for (++it; it != m_swcPack.swc().end(rit); ++it) {
      m_rootToChildren[&rit].push_back(it);
    }
  }
}

QVariant ZSwcTreeModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) {
    return QVariant();
  }

  auto item = *reinterpret_cast<ZSwc::ConstIterator*>(index.internalId());
  QString topologyType = "continuous";
  if (ZSwc::isRoot(item)) {
    topologyType = "Root";
  } else if (ZSwc::isBranchNode(item)) {
    topologyType = "Branch Node";
  } else if (ZSwc::isLeaf(item)) {
    topologyType = "Leaf";
  }

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case IDColumn:
        return item->id;
      case TypeColumn:
        return item->type;
      case XColumn:
        return item->x;
      case YColumn:
        return item->y;
      case ZColumn:
        return item->z;
      case RadiusColumn:
        return item->radius;
      case TopologyColumn:
        return topologyType;
      case ParentIDColumn:
        return item->parentID;
      case LabelColumn:
        return item->label;
      default:
        break;
    }
  }

  return QVariant();
}

QVariant ZSwcTreeModel::headerData(int section, Qt::Orientation orientation,
                                   int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case IDColumn:
        return "ID";
      case TypeColumn:
        return "Type";
      case XColumn:
        return "X";
      case YColumn:
        return "Y";
      case ZColumn:
        return "Z";
      case RadiusColumn:
        return "Radius";
      case TopologyColumn:
        return "Topology";
      case ParentIDColumn:
        return "Parent ID";
      case LabelColumn:
        return "Label";
      default:
        break;
    }
  }

  return QVariant();
}

QModelIndex ZSwcTreeModel::index(int row, int column, const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent)) {
    return QModelIndex();
  }

  if (!parent.isValid()) {
    CHECK(row < static_cast<int>(m_roots.size()));
    return createIndex(row, column, reinterpret_cast<std::uintptr_t>(&m_roots[row]));
  } else {
    CHECK(parent.row() < static_cast<int>(m_roots.size())) << m_roots.size() << " " << parent.row();
    CHECK(row < static_cast<int>(m_rootToChildren.at(&m_roots[parent.row()]).size()))
        << m_rootToChildren.at(&m_roots[parent.row()]).size() << " " << row;
    auto res = createIndex(row, column,
                           reinterpret_cast<std::uintptr_t>(&m_rootToChildren.at(&m_roots[parent.row()])[row]));
    return res;
  }
}

QModelIndex ZSwcTreeModel::parent(const QModelIndex& index) const
{
  if (!index.isValid()) {
    return QModelIndex();
  }

  auto it = *reinterpret_cast<ZSwc::ConstIterator*>(index.internalId());
  if (ZSwc::isRoot(it)) {
    return QModelIndex();
  }

  auto pit = ZSwc::parent(it);
  while (!ZSwc::isRoot(pit)) {
    pit = ZSwc::parent(pit);
  }
  for (size_t r = 0; r < m_roots.size(); ++r) {
    if (pit == m_roots[r]) {
      return createIndex(r, 0, reinterpret_cast<std::uintptr_t>(&m_roots[r]));
    }
  }

  CHECK(false);

  return QModelIndex();
}

int ZSwcTreeModel::rowCount(const QModelIndex& parent) const
{
  if (parent.column() > 0) {
    return 0;
  }

  if (!parent.isValid()) {
    return m_roots.size();
  } else {
    auto it = *reinterpret_cast<ZSwc::ConstIterator*>(parent.internalId());
    if (!ZSwc::isRoot(it)) {
      return 0;
    }
    CHECK(parent.row() < static_cast<int>(m_roots.size())) << m_roots.size() << " " << parent.row();
    return m_rootToChildren.at(&m_roots[parent.row()]).size();
  }
}

int ZSwcTreeModel::columnCount(const QModelIndex& /*parent*/) const
{
  return ColumnCount;
}

void ZSwcTreeModel::clicked(const QModelIndex& /*idxIn*/)
{
  //  if (idxIn.isValid()) {
  //    if (idxIn.column() == ViewSettingColumn) {
  //      ObjItem *item = static_cast<ObjItem*>(idxIn.internalID());
  //      if (m_viewSettingCurrentItem == item) {
  //        m_viewSettingCurrentItem = nullptr;
  //        m_regionAnnotation->sendHideViewSettingSignal();
  //      } else {
  //        QModelIndex prevIdx;
  //        for (int row = 0; row < rowCount(); ++row) {
  //          QModelIndex idx = index(row, ViewSettingColumn);
  //          if (static_cast<ObjItem*>(idx.internalID()) == m_viewSettingCurrentItem) {
  //            prevIdx = idx;
  //            break;
  //          }
  //          if (rowCount(idx) > 0) {
  //            for (int subRow = 0; subRow < rowCount(idx); ++subRow) {
  //              QModelIndex subIdx = index(subRow, ViewSettingColumn, idx);
  //              if (static_cast<ObjItem*>(subIdx.internalID()) == m_viewSettingCurrentItem) {
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

void ZSwcTreeModel::doubleClicked(const QModelIndex& index)
{
  if (!index.isValid()) {
    return;
  }

  auto p = *reinterpret_cast<ZSwc::ConstIterator*>(index.internalId());
  emit m_doc.requestToAdjustViewToPosition(p->x, p->y, p->z, 128);
}

void ZSwcTreeModel::activated(const QModelIndex& /*idxIn*/)
{
  //  size_t id = indexToId(idxIn);
  //  if (id > 0) {
  //    //LOG(INFO) << id;
  //    m_regionAnnotation->sendOpenEditWidgetSignal(id);
  //  }
}

} // namespace nim

