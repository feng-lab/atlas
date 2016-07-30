#include "zobjmodel.h"
#include "zobjdoc.h"
#include <cassert>
#include "zlog.h"

namespace nim {

ZObjModel::ZObjModel(ZDoc* doc)
  : QAbstractItemModel(doc)
  , m_doc(doc)
  , m_settingIcon(":/icons/settings-512.png")
  , m_visibleIcon(":/icons/visible.png")
  , m_invisibleIcon(":/icons/invisible.png")
  , m_lockIcon(":/icons/lock-512.png")
  , m_unlockIcon(":/icons/unlock-512.png")
  , m_rootItem(std::make_unique<ObjItem>(0, nullptr, nullptr))
  , m_viewSettingCurrentItem(nullptr)
{
}

QVariant ZObjModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return QVariant();

  ObjItem* item = static_cast<ObjItem*>(index.internalPointer());

  if (role == Qt::CheckStateRole && index.column() == ShowHideNameColumn && needCheckbox(index))
    return item->checkState;

  if (role == Qt::ToolTipRole)
    return item->doc->objTooltip(item->id);

  if (role == Qt::DisplayRole) {
    switch (index.column()) {
      case ShowHideNameColumn:
        return item->doc->objNameWithModifiedMarker(item->id);
        break;
      case ShowHideColumn:
        return "";
        break;
      case LockColumn:
        return "";
        break;
      case NameColumn:
        return item->doc->objNameWithModifiedMarker(item->id);
        break;
      case TypeColumn:
        return item->doc->typeName();
        break;
      case InfoColumn:
        return item->doc->objInfo(item->id);
        break;
      case IDColumn:
        return static_cast<qlonglong>(item->id);
        break;
      case ViewSettingColumn:
        return "";
        break;
      default:
        break;
    }
  }

  if (role == Qt::DecorationRole) {
    switch (index.column()) {
      case ShowHideColumn:
        return item->show ? m_visibleIcon : m_invisibleIcon;
      case LockColumn:
        return item->locked ? m_lockIcon : m_unlockIcon;
      case ViewSettingColumn:
        return m_settingIcon;
        break;
      default:
        break;
    }
  }

  if (role == Qt::BackgroundColorRole) {
    switch (index.column()) {
      case ViewSettingColumn:
        if (item == m_viewSettingCurrentItem)
          return QColor(255, 0, 0);
        break;
      default:
        break;
    }
  }


  return QVariant();
}

Qt::ItemFlags ZObjModel::flags(const QModelIndex& index) const
{
  if (!index.isValid())
    return 0;

  Qt::ItemFlags flags = Qt::ItemIsEnabled;

  if (index.column() != ShowHideColumn && index.column() != LockColumn)
    flags |= Qt::ItemIsSelectable;

  if (index.column() == ShowHideNameColumn)
    flags |= Qt::ItemIsUserCheckable;

  return flags;
}

bool ZObjModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid())
    return false;

  if (role == Qt::CheckStateRole && index.column() == ShowHideNameColumn && needCheckbox(index)) {
    Qt::CheckState cs = static_cast<Qt::CheckState>(value.toInt());

    setModelIndexCheckState(index, cs);
    // update child items check state
    updateChildCheckState(index, cs);
    // update parent items
    updateParentCheckState(index);

    return true;
  }

  return false;
}

QVariant ZObjModel::headerData(int section, Qt::Orientation orientation,
                               int role) const
{
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
    switch (section) {
      case ShowHideNameColumn:
        return QString("Name");
        break;
      case ShowHideColumn:
      case LockColumn:
        return QString();
        break;
      case NameColumn:
        return QString("Name");
        break;
      case TypeColumn:
        return QString("Type");
        break;
      case InfoColumn:
        return QString("Info");
        break;
      case IDColumn:
        return QString("ID");
        break;
      case ViewSettingColumn:
        return QString("View Settings");
        break;
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
        break;
      case ViewSettingColumn:
        return 1;
        break;
      default:
        break;
    }
  }

  return QVariant();
}

QModelIndex ZObjModel::index(int row, int column, const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent))
    return QModelIndex();

  ObjItem* parentItem;

  if (!parent.isValid())
    parentItem = m_rootItem.get();
  else
    parentItem = static_cast<ObjItem*>(parent.internalPointer());

  auto& childItem = parentItem->children[row];
  if (childItem)
    return createIndex(row, column, childItem.get());
  else
    return QModelIndex();
}

QModelIndex ZObjModel::parent(const QModelIndex& index) const
{
  if (!index.isValid())
    return QModelIndex();

  ObjItem* childItem = static_cast<ObjItem*>(index.internalPointer());
  ObjItem* parentItem = childItem->parent;

  if (parentItem == m_rootItem.get())
    return QModelIndex();

  return createIndex(parentItem->row(), 0, parentItem);
}

int ZObjModel::rowCount(const QModelIndex& parent) const
{
  ObjItem* parentItem;
  if (parent.column() > 0)
    return 0;

  if (!parent.isValid()) {
    parentItem = m_rootItem.get();
    return parentItem->children.size();
  } else {
    parentItem = static_cast<ObjItem*>(parent.internalPointer());
    return parentItem->children.size();
  }
}

int ZObjModel::columnCount(const QModelIndex&) const
{
#ifdef CONFIG_OLD
  return ColumnCount - 4;
#else
  return ColumnCount - 2;
#endif
}

size_t ZObjModel::numObjs() const
{
  int num = 0;
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    num += std::max<size_t>(m_rootItem->children[i]->children.size(), 1);
  }
  return num;
}

QList<size_t> ZObjModel::objs() const
{
  QList<size_t> res;
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    if (m_rootItem->children[i]->children.empty()) {
      res.push_back(m_rootItem->children[i]->id);
    } else {
      for (size_t j = 0; j < m_rootItem->children[i]->children.size(); ++j) {
        res.push_back(m_rootItem->children[i]->children[j]->id);
      }
    }
  }
  return res;
}

QList<size_t> ZObjModel::objsOfDoc(const ZObjDoc* doc) const
{
  QList<size_t> res;
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    if (m_rootItem->children[i]->doc != doc)
      continue;
    if (m_rootItem->children[i]->children.empty()) {
      res.push_back(m_rootItem->children[i]->id);
    } else {
      for (size_t j = 0; j < m_rootItem->children[i]->children.size(); ++j) {
        res.push_back(m_rootItem->children[i]->children[j]->id);
      }
    }
  }
  return res;
}

bool ZObjModel::isObjVisible(size_t id) const
{
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    if (m_rootItem->children[i]->children.empty()) {
      if (m_rootItem->children[i]->id == id)
        //return m_rootItem->children[i]->checkState == Qt::Checked;
        return m_rootItem->children[i]->show;
    } else {
      for (size_t j = 0; j < m_rootItem->children[i]->children.size(); ++j) {
        if (m_rootItem->children[i]->children[j]->id == id)
          //return m_rootItem->children[i]->children[j]->checkState == Qt::Checked;
          return m_rootItem->children[i]->children[j]->show;
      }
    }
  }
  return false;
}

void ZObjModel::setObjVisible(size_t id, bool v)
{
  if (isObjVisible(id) != v) {
    QModelIndex idx = idToIndex(id, 0);
    if (idx.isValid()) {
      setData(idx, v ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
      ObjItem* item = static_cast<ObjItem*>(idx.internalPointer());
      item->show = v;
      emit dataChanged(idx, idx);
    }
  }
}

ZObjDoc* ZObjModel::idToDoc(size_t id) const
{
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    if (m_rootItem->children[i]->children.empty()) {
      if (m_rootItem->children[i]->id == id)
        return m_rootItem->children[i]->doc;
    } else {
      for (size_t j = 0; j < m_rootItem->children[i]->children.size(); ++j) {
        if (m_rootItem->children[i]->children[j]->id == id)
          return m_rootItem->children[i]->children[j]->doc;
      }
    }
  }
  return nullptr;
}

void ZObjModel::updateObj(size_t id)
{
  emit dataChanged(idToIndex(id, 0), idToIndex(id, 3));
}

void ZObjModel::addObj(size_t id, ZObjDoc* doc)
{
  int row = m_rootItem->children.size();
  emit beginInsertRows(QModelIndex(), row, row);
  m_rootItem->children.emplace_back(std::make_unique<ObjItem>(id, doc, m_rootItem.get()));
  emit endInsertRows();
}

void ZObjModel::removeObj(size_t id)
{
  for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
    if (m_rootItem->children[i]->id == id) {
      emit beginRemoveRows(QModelIndex(), i, i);
      if (m_rootItem->children[i].get() == m_viewSettingCurrentItem) {
        m_viewSettingCurrentItem = nullptr;
        m_doc->sendHideViewSettingSignal();
      }
      m_rootItem->children.erase(m_rootItem->children.begin() + i);
      emit endRemoveRows();
      return;
    }
  }
}

void ZObjModel::removeObjsOfDoc(ZObjDoc* doc)
{
  bool cont = true;
  while (cont) {
    cont = false;
    for (size_t i = 0; i < m_rootItem->children.size(); ++i) {
      if (m_rootItem->children[i]->doc == doc) {
        emit beginRemoveRows(QModelIndex(), i, i);
        if (m_rootItem->children[i].get() == m_viewSettingCurrentItem) {
          m_viewSettingCurrentItem = nullptr;
          m_doc->sendHideViewSettingSignal();
        }
        m_rootItem->children.erase(m_rootItem->children.begin() + i);
        emit endRemoveRows();
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
  m_viewSettingCurrentItem = nullptr;
  m_doc->sendHideViewSettingSignal();
  endResetModel();
}

size_t ZObjModel::indexToId(const QModelIndex& index)
{
  return index.isValid() ? static_cast<ObjItem*>(index.internalPointer())->id : 0;
}

QModelIndex ZObjModel::idToIndex(size_t id, int col)
{
  QModelIndex res;
  for (int row = 0; row < rowCount(); ++row) {
    QModelIndex idx = index(row, col);
    ObjItem* objItem = static_cast<ObjItem*>(idx.internalPointer());
    if (objItem->id == id) {
      res = idx;
      break;
    }
    if (rowCount(idx) > 0) {
      for (int subRow = 0; subRow < rowCount(idx); ++subRow) {
        QModelIndex subIdx = index(subRow, col, idx);
        objItem = static_cast<ObjItem*>(subIdx.internalPointer());
        if (objItem->id == id) {
          res = subIdx;
          break;
        }
      }
    }
    if (res.isValid())
      break;
  }
  return res;
}

ZObjDoc* ZObjModel::indexToDoc(const QModelIndex& index)
{
  return index.isValid() ? static_cast<ObjItem*>(index.internalPointer())->doc : nullptr;
}

void ZObjModel::clicked(const QModelIndex& idxIn)
{
  if (idxIn.isValid()) {
    if (idxIn.column() == ShowHideNameColumn || idxIn.column() == NameColumn) {
      ObjItem* item = static_cast<ObjItem*>(idxIn.internalPointer());
      m_doc->sendShowViewSettingSignal(item->id);
    } else if (idxIn.column() == ShowHideColumn) {
      ObjItem* item = static_cast<ObjItem*>(idxIn.internalPointer());
      item->show = !item->show;
      emit dataChanged(idxIn, idxIn);
      item->doc->setObjVisible(item->id, item->show);
    } else if (idxIn.column() == LockColumn) {
      ObjItem* item = static_cast<ObjItem*>(idxIn.internalPointer());
      item->locked = !item->locked;
      emit dataChanged(idxIn, idxIn);
    } else if (idxIn.column() == ViewSettingColumn) {
      ObjItem* item = static_cast<ObjItem*>(idxIn.internalPointer());
      if (m_viewSettingCurrentItem == item) {
        m_viewSettingCurrentItem = nullptr;
        m_doc->sendHideViewSettingSignal();
      } else {
        QModelIndex prevIdx;
        for (int row = 0; row < rowCount(); ++row) {
          QModelIndex idx = index(row, ViewSettingColumn);
          if (static_cast<ObjItem*>(idx.internalPointer()) == m_viewSettingCurrentItem) {
            prevIdx = idx;
            break;
          }
          if (rowCount(idx) > 0) {
            for (int subRow = 0; subRow < rowCount(idx); ++subRow) {
              QModelIndex subIdx = index(subRow, ViewSettingColumn, idx);
              if (static_cast<ObjItem*>(subIdx.internalPointer()) == m_viewSettingCurrentItem) {
                prevIdx = subIdx;
                break;
              }
            }
          }
          if (prevIdx.isValid())
            break;
        }

        m_viewSettingCurrentItem = item;
        if (prevIdx.isValid())
          emit dataChanged(prevIdx, prevIdx);
        m_doc->sendShowViewSettingSignal(item->id);
      }
      emit dataChanged(idxIn, idxIn);
    }
  }
}

void ZObjModel::doubleClicked(const QModelIndex&)
{
}

void ZObjModel::activated(const QModelIndex& idxIn)
{
  if (idxIn.column() == ShowHideNameColumn || idxIn.column() == NameColumn) {
    size_t id = indexToId(idxIn);
    if (id > 0) {
      //LOG(INFO) << id;
      m_doc->sendOpenEditWidgetSignal(id);
    }
  }
}

void ZObjModel::updateChildCheckState(const QModelIndex& parent, Qt::CheckState cs)
{
  CHECK(cs != Qt::PartiallyChecked);
  for (int i = 0; i < rowCount(parent); i++) {
    QModelIndex child = index(i, ShowHideNameColumn, parent);
    if (!needCheckbox(child))
      return;
    if (cs != getModelIndexCheckState(child)) {
      setModelIndexCheckState(child, cs);
      updateChildCheckState(child, cs);
    }
  }
}

void ZObjModel::updateParentCheckState(const QModelIndex& child)
{
  QModelIndex idx = parent(child);
  if (!idx.isValid())
    return;

  int numChecked = 0;
  int numUnChecked = 0;
  int numPartialChecked = 0;
  Qt::CheckState oldCheckState = getModelIndexCheckState(idx);
  Qt::CheckState newCheckState;

  for (int i = 0; i < rowCount(idx); i++) {
    QModelIndex cld = index(i, ShowHideNameColumn, idx);
    Qt::CheckState cs = getModelIndexCheckState(cld);
    if (cs == Qt::Checked) {
      ++numChecked;
    } else if (cs == Qt::Unchecked) {
      ++numUnChecked;
    } else {  // should be PartialChecked
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
  } else {   // numUnChecked == 0
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
  ObjItem* item = static_cast<ObjItem*>(index.internalPointer());
  item->checkState = cs;
  emit dataChanged(index, index);
  item->doc->setObjVisible(item->id, item->checkState == Qt::Checked);
}

Qt::CheckState ZObjModel::getModelIndexCheckState(const QModelIndex& index) const
{
  return static_cast<ObjItem*>(index.internalPointer())->checkState;
}

bool ZObjModel::needCheckbox(const QModelIndex& index) const
{
  if (static_cast<ObjItem*>(index.internalPointer()) == m_rootItem.get()) {
    return false;
  }
  return true;
}

ZObjModel::ObjItem::ObjItem(size_t id, ZObjDoc* doc, ObjItem* parent)
  : parent(parent), id(id), doc(doc), locked(false), show(true)
{
  checkState = Qt::Checked;
}

int ZObjModel::ObjItem::row() const
{
  if (parent) {
    for (size_t i = 0; i < parent->children.size(); ++i) {
      if (parent->children[i].get() == this) {
        return i;
      }
    }
  }
  return 0;
}

} // namespace nim
