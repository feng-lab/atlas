#pragma once

#include <QItemSelectionModel>

class QAbstractProxyModel;

namespace nim {

class ZShareItemSelectionModel : public QItemSelectionModel
{
Q_OBJECT
public:
  explicit ZShareItemSelectionModel(QAbstractItemModel* model,
                                    QItemSelectionModel* srcSelectionModel, QObject* parent = nullptr);

  // QItemSelectionModel interface
public:
  void select(const QModelIndex& index, SelectionFlags command) override;

  void select(const QItemSelection& selection, SelectionFlags command) override;

private:
  void srcSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

  void srcCurrentChanged(const QModelIndex& srcCurrent, const QModelIndex& previous);

  void thisCurrentChanged(const QModelIndex& current, const QModelIndex& previous);

  [[nodiscard]] QModelIndex mapIndexToSrc(const QModelIndex& index) const;

  [[nodiscard]] QModelIndex mapIndexFromSrc(const QModelIndex& index) const;

  [[nodiscard]] QItemSelection mapSelectionToSrc(const QItemSelection& selection) const;

  [[nodiscard]] QItemSelection mapSelectionFromSrc(const QItemSelection& selection) const;

private:
  QItemSelectionModel* m_srcSelectionModel;
  QList<const QAbstractProxyModel*> m_proxyChain;
};

} // namespace nim

