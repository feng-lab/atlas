#include "zshareitemselectionmodel.h"

#include "zlog.h"
#include "zglobal.h"
#include <QAbstractProxyModel>

namespace nim {

ZShareItemSelectionModel::ZShareItemSelectionModel(QAbstractItemModel* model,
                                                   QItemSelectionModel* srcSelectionModel, QObject* parent)
  : QItemSelectionModel(model, parent)
  , m_srcSelectionModel(srcSelectionModel)
{
  const QAbstractItemModel* srcModel = m_srcSelectionModel->model();
  if (srcModel != model) {
    const QAbstractProxyModel* proxyModel = qobject_cast<const QAbstractProxyModel*>(model);
    CHECK(proxyModel);
    while (true) {
      m_proxyChain.prepend(proxyModel);
      if (proxyModel->sourceModel() == srcModel) {
        break;
      } else {
        proxyModel = qobject_cast<const QAbstractProxyModel*>(proxyModel->sourceModel());
        CHECK(proxyModel);
      }
    }
  }

  QItemSelectionModel::select(mapSelectionFromSrc(m_srcSelectionModel->selection()), QItemSelectionModel::Select);

  connect(m_srcSelectionModel, &QItemSelectionModel::currentChanged,
          this, &ZShareItemSelectionModel::srcCurrentChanged);
  connect(m_srcSelectionModel, &QItemSelectionModel::selectionChanged,
          this, &ZShareItemSelectionModel::srcSelectionChanged);
  connect(this, &ZShareItemSelectionModel::currentChanged,
          this, &ZShareItemSelectionModel::thisCurrentChanged);
}

void ZShareItemSelectionModel::select(const QModelIndex& index, SelectionFlags command)
{
  QItemSelectionModel::select(QItemSelection(index, index), command);
  m_srcSelectionModel->select(mapSelectionToSrc(QItemSelection(index, index)), command);
}

void ZShareItemSelectionModel::select(const QItemSelection& selection, SelectionFlags command)
{
  QItemSelectionModel::select(selection, command);
  m_srcSelectionModel->select(mapSelectionToSrc(selection), command);
}

void ZShareItemSelectionModel::srcSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  QItemSelectionModel::select(mapSelectionFromSrc(selected), QItemSelectionModel::Select);
  QItemSelectionModel::select(mapSelectionFromSrc(deselected), QItemSelectionModel::Deselect);
}

void ZShareItemSelectionModel::srcCurrentChanged(const QModelIndex& srcCurrent, const QModelIndex& /*unused*/)
{
  const QModelIndex current = mapIndexFromSrc(srcCurrent);
  if (!current.isValid() || current == currentIndex())
    return;
  setCurrentIndex(current, QItemSelectionModel::NoUpdate);
}

void ZShareItemSelectionModel::thisCurrentChanged(const QModelIndex& current, const QModelIndex& /*unused*/)
{
  const QModelIndex srcCurrent = mapIndexToSrc(current);
  if (!srcCurrent.isValid() || srcCurrent == m_srcSelectionModel->currentIndex())
    return;
  m_srcSelectionModel->setCurrentIndex(srcCurrent, QItemSelectionModel::NoUpdate);
}

QModelIndex ZShareItemSelectionModel::mapIndexToSrc(const QModelIndex& index) const
{
  if (!index.isValid())
    return QModelIndex();
  QModelIndex res = index;
  for (index_t i = m_proxyChain.size(); i-- > 0;) {
    res = m_proxyChain[i]->mapToSource(res);
  }
  return res;
}

QModelIndex ZShareItemSelectionModel::mapIndexFromSrc(const QModelIndex& index) const
{
  if (!index.isValid())
    return QModelIndex();
  QModelIndex res = index;
  for (index_t i = 0; i < m_proxyChain.size(); ++i) {
    res = m_proxyChain[i]->mapFromSource(res);
  }
  return res;
}

QItemSelection ZShareItemSelectionModel::mapSelectionToSrc(const QItemSelection& selection) const
{
  if (selection.isEmpty())
    return QItemSelection();
  QItemSelection res = selection;
  for (index_t i = m_proxyChain.size(); i-- > 0;) {
    res = m_proxyChain[i]->mapSelectionToSource(res);
  }
  return res;
}

QItemSelection ZShareItemSelectionModel::mapSelectionFromSrc(const QItemSelection& selection) const
{
  if (selection.isEmpty())
    return QItemSelection();
  QItemSelection res = selection;
  for (index_t i = 0; i < m_proxyChain.size(); ++i) {
    res = m_proxyChain[i]->mapSelectionFromSource(res);
  }
  return res;
}

} // namespace nim
