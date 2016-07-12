#ifndef ZSHAREITEMSELECTIONMODEL_H
#define ZSHAREITEMSELECTIONMODEL_H

#include <QItemSelectionModel>

class QAbstractProxyModel;

namespace nim {

class ZShareItemSelectionModel : public QItemSelectionModel
{
  Q_OBJECT
public:
  explicit ZShareItemSelectionModel(QAbstractItemModel *model,
                                    QItemSelectionModel *srcSelectionModel, QObject *parent = 0);

  // QItemSelectionModel interface
public:
  virtual void select(const QModelIndex &index, SelectionFlags command) override;
  virtual void select(const QItemSelection &selection, SelectionFlags command) override;

private:
  void srcSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
  void srcCurrentChanged(const QModelIndex &current, const QModelIndex &previous);
  void thisCurrentChanged(const QModelIndex &current, const QModelIndex &previous);

  QModelIndex mapIndexToSrc(const QModelIndex &index) const;
  QModelIndex mapIndexFromSrc(const QModelIndex &index) const;
  QItemSelection mapSelectionToSrc(const QItemSelection &selection) const;
  QItemSelection mapSelectionFromSrc(const QItemSelection &selection) const;

private:
  QItemSelectionModel *m_srcSelectionModel;
  QList<const QAbstractProxyModel*> m_proxyChain;
};

} // namespace nim

#endif // ZSHAREITEMSELECTIONMODEL_H
