#include "zobjmodel.h"

#include "zobjdoc.h"
#include "zlog.h"
#include "ztheme.h"
#include <algorithm>

namespace nim {

ZObjModel::ZObjModel(ZDoc* doc)
  : QAbstractItemModel(doc)
  , m_doc(doc)
  , m_rootItem(std::make_unique<ZObjPack>(0, nullptr, nullptr))
  , m_viewSettingCurrentItem(nullptr)
{}

ZObjPack* ZObjModel::packForId(size_t id) const
{
  if (id == 0) {
    return nullptr;
  }

  auto it = m_idToPack.find(id);
  if (it == m_idToPack.end()) {
    return nullptr;
  }

  return it->second;
}

void ZObjModel::indexPackTree(ZObjPack* pack)
{
  CHECK(pack);

  if (pack != m_rootItem.get() && pack->m_id != 0) {
    const bool inserted = m_idToPack.emplace(pack->m_id, pack).second;
    CHECK(inserted);
  }

  for (auto& child : pack->children) {
    CHECK(child);
    if (child->parent == nullptr) {
      child->parent = pack;
    } else {
      CHECK(child->parent == pack);
    }
    indexPackTree(child.get());
  }
}

void ZObjModel::unindexPackTree(const ZObjPack* pack)
{
  CHECK(pack);

  for (const auto& child : pack->children) {
    CHECK(child);
    unindexPackTree(child.get());
  }

  if (pack != m_rootItem.get() && pack->m_id != 0) {
    const size_t erased = m_idToPack.erase(pack->m_id);
    CHECK(erased == 1);
  }
}

QModelIndex ZObjModel::indexForPack(const ZObjPack& pack, int col) const
{
  if (&pack == m_rootItem.get()) {
    return QModelIndex();
  }

  CHECK(pack.parent);
  return createIndex(pack.row(), col, const_cast<ZObjPack*>(&pack));
}

QVariant ZObjModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) {
    return QVariant();
  }

  auto item = static_cast<ZObjPack*>(index.internalPointer());

  if (role == Qt::CheckStateRole && index.column() == ShowHideNameColumn && needCheckbox(index)) {
    return item->m_show ? Qt::Checked : Qt::Unchecked;
  }

  if (role == Qt::ToolTipRole) {
    switch (index.column()) {
      case LockColumn:
        return "Lock from editing";
      default:
        return item->m_objDoc->objTooltip(item->m_id);
    }
  }

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case ShowHideNameColumn:
        return item->m_objDoc->objNameWithModifiedMarker(item->m_id);
      case LockColumn:
        return "";
      case TypeColumn:
        return item->m_objDoc->typeName();
      case InfoColumn:
        return item->m_objDoc->objInfo(item->m_id);
      case IDColumn:
        return static_cast<qlonglong>(item->m_id);
      case ShowHideColumn:
        return "";
      case NameColumn:
        return item->m_objDoc->objNameWithModifiedMarker(item->m_id);
      case ViewSettingColumn:
        return "";
      default:
        break;
    }
  }

  if (role == Qt::DecorationRole) {
    switch (index.column()) {
      case ShowHideColumn:
        return item->m_show ? ZTheme::instance().icon(ZTheme::EyeOpenIcon)
                            : ZTheme::instance().icon(ZTheme::EyeCloseIcon);
      case LockColumn:
        return item->m_locked ? ZTheme::instance().icon(ZTheme::LockedIcon)
                              : ZTheme::instance().icon(ZTheme::UnlockedIcon);
      case ViewSettingColumn:
        return ZTheme::instance().icon(ZTheme::SettingsIcon);
      default:
        break;
    }
  }

  if (role == Qt::BackgroundRole) {
    switch (index.column()) {
      case ViewSettingColumn:
        if (item == m_viewSettingCurrentItem) {
          return QColor(255, 0, 0);
        }
        break;
      default:
        break;
    }
  }

  return QVariant();
}

Qt::ItemFlags ZObjModel::flags(const QModelIndex& index) const
{
  if (!index.isValid()) {
    return Qt::ItemFlags();
  }

  Qt::ItemFlags flags = Qt::ItemIsEnabled;

  if (index.column() != ShowHideColumn && index.column() != LockColumn) {
    flags |= Qt::ItemIsSelectable;
  }

  if (index.column() == ShowHideNameColumn) {
    flags |= Qt::ItemIsUserCheckable;
  }

  return flags;
}

bool ZObjModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid()) {
    return false;
  }

  if (role == Qt::CheckStateRole && index.column() == ShowHideNameColumn && needCheckbox(index)) {
    auto cs = static_cast<Qt::CheckState>(value.toInt());

    setModelIndexCheckState(index, cs);
    // update child items check state
    updateChildCheckState(index, cs);
    // update parent items
    updateParentCheckState(index);

    return true;
  }

  return false;
}

QVariant ZObjModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case ShowHideNameColumn:
        return QString("(Show/Hide) Name");
      case LockColumn:
        return QString("Lock");
      case TypeColumn:
        return QString("Type");
      case InfoColumn:
        return QString("Info");
      case IDColumn:
        return QString("ID");
      case ShowHideColumn:
        return QString();
      case NameColumn:
        return QString("Name");
      case ViewSettingColumn:
        return QString("View Settings");
      default:
        break;
    }
  }

  if (orientation == Qt::Horizontal && role == Qt::UserRole) {
    switch (section) {
      case ShowHideNameColumn:
      case NameColumn:
      case ShowHideColumn:
      case LockColumn:
      case TypeColumn:
      case InfoColumn:
      case IDColumn:
        return 0;
      case ViewSettingColumn:
        return 1;
      default:
        break;
    }
  }

  return QVariant();
}

QModelIndex ZObjModel::index(int row, int column, const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent)) {
    return QModelIndex();
  }

  ZObjPack* parentItem;
  if (!parent.isValid()) {
    parentItem = m_rootItem.get();
  } else {
    parentItem = static_cast<ZObjPack*>(parent.internalPointer());
  }

  auto& childItem = parentItem->children[row];
  if (childItem) {
    return createIndex(row, column, childItem.get());
  } else {
    return QModelIndex();
  }
}

QModelIndex ZObjModel::parent(const QModelIndex& index) const
{
  if (!index.isValid()) {
    return QModelIndex();
  }

  auto childItem = static_cast<ZObjPack*>(index.internalPointer());
  auto parentItem = childItem->parent;

  if (parentItem == m_rootItem.get()) {
    return QModelIndex();
  }

  return createIndex(parentItem->row(), 0, parentItem);
}

int ZObjModel::rowCount(const QModelIndex& parent) const
{
  if (parent.column() > 0) {
    return 0;
  }

  if (!parent.isValid()) {
    auto parentItem = m_rootItem.get();
    return parentItem->children.size();
  } else {
    auto parentItem = static_cast<ZObjPack*>(parent.internalPointer());
    return parentItem->children.size();
  }
}

int ZObjModel::columnCount(const QModelIndex& /*parent*/) const
{
#ifdef CONFIG_OLD
  return ColumnCount - 3;
#else
  return ColumnCount - 2;
#endif
}

size_t ZObjModel::numObjs() const
{
  int num = 0;
  for (const auto& c : m_rootItem->children) {
    num += std::max<size_t>(c->children.size(), 1);
  }
  return num;
}

std::vector<size_t> ZObjModel::objs() const
{
  std::vector<size_t> res;
  for (const auto& c : m_rootItem->children) {
    if (c->children.empty()) {
      res.push_back(c->m_id);
    } else {
      for (const auto& cc : c->children) {
        res.push_back(cc->m_id);
      }
    }
  }
  return res;
}

std::vector<size_t> ZObjModel::objsOfDoc(const ZObjDoc* doc) const
{
  std::vector<size_t> res;
  for (const auto& c : m_rootItem->children) {
    if (c->m_objDoc != doc) {
      continue;
    }
    if (c->children.empty()) {
      res.push_back(c->m_id);
    } else {
      for (const auto& cc : c->children) {
        res.push_back(cc->m_id);
      }
    }
  }
  return res;
}

bool ZObjModel::isObjVisible(size_t id) const
{
  const ZObjPack* pack = packForId(id);
  return pack ? pack->m_show : false;
}

void ZObjModel::setObjVisible(size_t id, bool v)
{
  ZObjPack* pack = packForId(id);
  if (!pack) {
    return;
  }

  if (pack->m_show != v) {
    const QModelIndex idx = indexForPack(*pack, ShowHideNameColumn);
    CHECK(idx.isValid());
    setData(idx, v ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
  }
}

bool ZObjModel::isObjLocked(size_t id) const
{
  const ZObjPack* pack = packForId(id);
  return pack ? pack->m_locked : false;
}

void ZObjModel::setObjLocked(size_t id, bool v)
{
  ZObjPack* pack = packForId(id);
  if (!pack) {
    return;
  }

  if (pack->m_locked != v) {
    pack->setLocked(v);
    const QModelIndex idx = indexForPack(*pack, LockColumn);
    CHECK(idx.isValid());
    Q_EMIT dataChanged(idx, idx, {Qt::DecorationRole});
  }
}

ZObjDoc* ZObjModel::idToDoc(size_t id) const
{
  const ZObjPack* pack = packForId(id);
  return pack ? pack->m_objDoc : nullptr;
}

void ZObjModel::updateObj(size_t id)
{
  ZObjPack* pack = packForId(id);
  if (!pack) {
    return;
  }

  const QModelIndex left = indexForPack(*pack, 0);
  const int lastCol = columnCount(QModelIndex()) - 1;
  const QModelIndex right = indexForPack(*pack, std::min<int>(IDColumn, lastCol));
  CHECK(left.isValid());
  CHECK(right.isValid());
  Q_EMIT dataChanged(left, right, {Qt::DisplayRole, Qt::ToolTipRole});
}

void ZObjModel::addObj(size_t id, ZObjDoc& doc)
{
  int row = m_rootItem->children.size();
  Q_EMIT beginInsertRows(QModelIndex(), row, row);
  m_rootItem->children.emplace_back(std::make_unique<ZObjPack>(id, &doc));
  auto* pack = m_rootItem->children[row].get();
  pack->parent = m_rootItem.get();
  indexPackTree(pack);
  Q_EMIT endInsertRows();
}

void ZObjModel::addObj(const std::shared_ptr<ZObjPack>& obj)
{
  int row = m_rootItem->children.size();
  Q_EMIT beginInsertRows(QModelIndex(), row, row);
  obj->parent = m_rootItem.get();
  m_rootItem->children.push_back(obj);
  indexPackTree(obj.get());
  Q_EMIT endInsertRows();
}

void ZObjModel::removeObj(size_t id)
{
  ZObjPack* pack = packForId(id);
  if (!pack) {
    return;
  }

  ZObjPack* parentPack = pack->parent;
  CHECK(parentPack);

  const QModelIndex parentIndex =
    (parentPack == m_rootItem.get()) ? QModelIndex() : indexForPack(*parentPack, ShowHideNameColumn);

  const int row = pack->row();
  CHECK(row >= 0);
  CHECK(row < static_cast<int>(parentPack->children.size()));

  Q_EMIT beginRemoveRows(parentIndex, row, row);
  if (pack == m_viewSettingCurrentItem) {
    m_viewSettingCurrentItem = nullptr;
    m_doc->sendHideViewSettingSignal();
  }
  unindexPackTree(pack);
  parentPack->children.erase(parentPack->children.begin() + row);
  Q_EMIT endRemoveRows();
}

void ZObjModel::removeObjsOfDoc(ZObjDoc* doc)
{
  bool cont = true;
  while (cont) {
    cont = false;
    for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
      if (m_rootItem->children[i]->m_objDoc == doc) {
        Q_EMIT beginRemoveRows(QModelIndex(), i, i);
        if (m_rootItem->children[i].get() == m_viewSettingCurrentItem) {
          m_viewSettingCurrentItem = nullptr;
          m_doc->sendHideViewSettingSignal();
        }
        unindexPackTree(m_rootItem->children[i].get());
        m_rootItem->children.erase(m_rootItem->children.begin() + i);
        Q_EMIT endRemoveRows();
        cont = true;
        break;
      }
    }
  }
}

void ZObjModel::removeAllObjs()
{
  beginResetModel();
  m_rootItem->children.clear();
  m_idToPack.clear();
  m_viewSettingCurrentItem = nullptr;
  m_doc->sendHideViewSettingSignal();
  endResetModel();
}

size_t ZObjModel::indexToId(const QModelIndex& index)
{
  return index.isValid() ? static_cast<ZObjPack*>(index.internalPointer())->m_id : 0;
}

QModelIndex ZObjModel::idToIndex(size_t id, int col)
{
  ZObjPack* pack = packForId(id);
  if (!pack) {
    return QModelIndex();
  }
  return indexForPack(*pack, col);
}

ZObjDoc* ZObjModel::indexToDoc(const QModelIndex& index)
{
  return index.isValid() ? static_cast<ZObjPack*>(index.internalPointer())->m_objDoc : nullptr;
}

void ZObjModel::clicked(const QModelIndex& idxIn)
{
  if (idxIn.isValid()) {
    if (idxIn.column() == ShowHideNameColumn || idxIn.column() == NameColumn) {
      auto item = static_cast<ZObjPack*>(idxIn.internalPointer());
      m_doc->sendShowViewSettingSignal(item->m_id);
    } else if (idxIn.column() == LockColumn) {
      auto item = static_cast<ZObjPack*>(idxIn.internalPointer());
      item->setLocked(!item->m_locked);
      Q_EMIT dataChanged(idxIn, idxIn, {Qt::DecorationRole});
    }
  }
}

void ZObjModel::doubleClicked(const QModelIndex&) {}

void ZObjModel::activated(const QModelIndex& idxIn)
{
  if (idxIn.column() == ShowHideNameColumn || idxIn.column() == NameColumn) {
    size_t id = indexToId(idxIn);
    if (id > 0) {
      // VLOG(1) << id;
      m_doc->sendOpenEditWidgetSignal(id);
    }
  }
}

void ZObjModel::updateChildCheckState(const QModelIndex& parent, Qt::CheckState cs)
{
  CHECK(cs != Qt::PartiallyChecked);
  for (int i = 0; i < rowCount(parent); ++i) {
    QModelIndex child = index(i, ShowHideNameColumn, parent);
    if (!needCheckbox(child)) {
      return;
    }
    if (cs != getModelIndexCheckState(child)) {
      setModelIndexCheckState(child, cs);
      updateChildCheckState(child, cs);
    }
  }
}

void ZObjModel::updateParentCheckState(const QModelIndex& child)
{
  QModelIndex idx = parent(child);
  if (!idx.isValid()) {
    return;
  }

  int numChecked = 0;
  int numUnChecked = 0;
  int numPartialChecked = 0;
  Qt::CheckState oldCheckState = getModelIndexCheckState(idx);
  Qt::CheckState newCheckState;

  for (int i = 0; i < rowCount(idx); ++i) {
    QModelIndex cld = index(i, ShowHideNameColumn, idx);
    Qt::CheckState cs = getModelIndexCheckState(cld);
    if (cs == Qt::Checked) {
      ++numChecked;
    } else if (cs == Qt::Unchecked) {
      ++numUnChecked;
    } else { // should be PartialChecked
      ++numPartialChecked;
      break;
    }
  }

  if (numPartialChecked > 0) {
    newCheckState = Qt::PartiallyChecked;
  } else if (numChecked > 0 && numUnChecked > 0) {
    newCheckState = Qt::PartiallyChecked;
  } else if (numChecked == 0) {
    CHECK(numUnChecked > 0);
    newCheckState = Qt::Unchecked;
  } else { // numUnChecked == 0
    CHECK(numChecked > 0);
    newCheckState = Qt::Checked;
  }

  if (newCheckState != oldCheckState) {
    setModelIndexCheckState(idx, newCheckState);
    updateParentCheckState(idx);
  }
}

void ZObjModel::setModelIndexCheckState(const QModelIndex& index, Qt::CheckState cs)
{
  auto item = static_cast<ZObjPack*>(index.internalPointer());
  item->setVisible(cs == Qt::Checked);
  Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
}

Qt::CheckState ZObjModel::getModelIndexCheckState(const QModelIndex& index) const
{
  return static_cast<ZObjPack*>(index.internalPointer())->m_show ? Qt::Checked : Qt::Unchecked;
}

bool ZObjModel::needCheckbox(const QModelIndex& index) const
{
  return index.internalPointer() != m_rootItem.get();
}

} // namespace nim
