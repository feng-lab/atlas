#include "zitemselectionmodel.h"

#include "zobjmodel.h"
#include "zobjdoc.h"

namespace nim {

ZItemSelectionModel::ZItemSelectionModel(ZObjModel* model, QObject* parent)
  : QItemSelectionModel(model, parent)
  , m_model(model)
{
  connect(this, &ZItemSelectionModel::selectionChanged, this, &ZItemSelectionModel::convertSelectionChangedSignal);
}

size_t ZItemSelectionModel::numSelectedObjs() const
{
  size_t num = 0;
  QModelIndexList indexes = selectedIndexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    ++num;
  }
  return num;
}

std::vector<size_t> ZItemSelectionModel::selectedObjs() const
{
  std::vector<size_t> res;
  QModelIndexList indexes = selectedIndexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    res.push_back(m_model->indexToId(index));
  }
  return res;
}

std::vector<size_t> ZItemSelectionModel::selectedObjsOfDoc(const ZObjDoc* objD) const
{
  std::vector<size_t> res;
  QModelIndexList indexes = selectedIndexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    if (m_model->indexToDoc(index) == objD) {
      res.push_back(m_model->indexToId(index));
    }
  }
  return res;
}

bool ZItemSelectionModel::isObjSelected(size_t id) const
{
  QModelIndexList indexes = selectedIndexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    if (id == m_model->indexToId(index)) {
      return true;
    }
  }
  return false;
}

void ZItemSelectionModel::setObjSelected(size_t id, bool v)
{
  QModelIndex index = m_model->idToIndex(id);
  if (index.isValid()) {
    select(index, v ? QItemSelectionModel::Select : QItemSelectionModel::Deselect);
  }
}

void ZItemSelectionModel::deselectObj(size_t id)
{
  QModelIndex index = m_model->idToIndex(id);
  if (index.isValid()) {
    select(index, QItemSelectionModel::Deselect);
  }
}

void ZItemSelectionModel::clearAndSelectObj(size_t id)
{
  QModelIndex index = m_model->idToIndex(id);
  if (index.isValid()) {
    select(index, QItemSelectionModel::ClearAndSelect);
  }
}

void ZItemSelectionModel::appendSelectObj(size_t id)
{
  QModelIndex index = m_model->idToIndex(id);
  if (index.isValid()) {
    select(index, QItemSelectionModel::Select);
  }
}

void ZItemSelectionModel::convertSelectionChangedSignal(const QItemSelection& selected,
                                                        const QItemSelection& deselected)
{
  std::map<ZObjDoc*, std::vector<size_t>> docSelected;
  std::map<ZObjDoc*, std::vector<size_t>> docDeselected;
  QModelIndexList indexes = selected.indexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    ZObjDoc* doc = m_model->indexToDoc(index);
    size_t id = m_model->indexToId(index);
    docSelected[doc].push_back(id);
    docDeselected.emplace(doc, std::vector<size_t>());
  }
  indexes = deselected.indexes();
  for (auto& index : indexes) {
    if (index.column() != ZObjModel::ShowHideNameColumn && index.column() != ZObjModel::NameColumn) {
      continue;
    }
    ZObjDoc* doc = m_model->indexToDoc(index);
    size_t id = m_model->indexToId(index);
    docDeselected[doc].push_back(id);
    if (!docSelected.contains(doc)) {
      docSelected.emplace(doc, std::vector<size_t>());
    }
  }
  auto it1 = docDeselected.begin();
  for (auto it = docSelected.begin(); it != docSelected.end(); ++it, ++it1) {
    it->first->sendObjSelectionChangedFromDocSignal(it->second, it1->second);
  }
}

} // namespace nim
