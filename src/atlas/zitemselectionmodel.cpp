#include "zitemselectionmodel.h"

#include "zobjmodel.h"
#include <QSet>
#include "zobjdoc.h"

namespace nim {

ZItemSelectionModel::ZItemSelectionModel(ZObjModel *model, QObject *parent)
  : QItemSelectionModel(model, parent)
  , m_model(model)
{
  connect(this, &ZItemSelectionModel::selectionChanged,
          this, &ZItemSelectionModel::convertSelectionChangedSignal);
}

size_t ZItemSelectionModel::numSelectedObjs() const
{
  size_t num = 0;
  QModelIndexList indexes = selectedIndexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    ++num;
  }
  return num;
}

QList<size_t> ZItemSelectionModel::selectedObjs() const
{
  QList<size_t> res;
  QModelIndexList indexes = selectedIndexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    res.push_back(m_model->indexToId(indexes[i]));
  }
  return res;
}

QList<size_t> ZItemSelectionModel::selectedObjsOfDoc(const ZObjDoc *objD) const
{
  QList<size_t> res;
  QModelIndexList indexes = selectedIndexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    if (m_model->indexToDoc(indexes[i]) == objD)
      res.push_back(m_model->indexToId(indexes[i]));
  }
  return res;
}

bool ZItemSelectionModel::isObjSelected(size_t id) const
{
  QModelIndexList indexes = selectedIndexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    if (id == m_model->indexToId(indexes[i]))
      return true;
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

void ZItemSelectionModel::convertSelectionChangedSignal(const QItemSelection &selected, const QItemSelection &deselected)
{
  std::map<ZObjDoc*,QList<size_t>> docSelected;
  std::map<ZObjDoc*,QList<size_t>> docDeselected;
  QModelIndexList indexes = selected.indexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    ZObjDoc* doc = m_model->indexToDoc(indexes[i]);
    size_t id = m_model->indexToId(indexes[i]);
    docSelected[doc].push_back(id);
    docDeselected.emplace(doc, QList<size_t>());
  }
  indexes = deselected.indexes();
  for (int i=0; i<indexes.size(); ++i) {
    if (indexes[i].column() != ZObjModel::ShowHideNameColumn && indexes[i].column() != ZObjModel::NameColumn)
      continue;
    ZObjDoc* doc = m_model->indexToDoc(indexes[i]);
    size_t id = m_model->indexToId(indexes[i]);
    docDeselected[doc].push_back(id);
    if (docSelected.find(doc) == docSelected.end())
      docSelected.emplace(doc, QList<size_t>());
  }
  std::map<ZObjDoc*,QList<size_t>>::iterator it1 = docDeselected.begin();
  for (std::map<ZObjDoc*,QList<size_t>>::iterator it = docSelected.begin();
       it != docSelected.end(); ++it, ++it1) {
    it->first->sendObjSelectionChangedFromDocSignal(it->second, it1->second);
  }
}

} // namespace nim
