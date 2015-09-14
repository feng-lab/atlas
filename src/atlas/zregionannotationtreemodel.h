#ifndef ZREGIONANNOTATIONTREEMODEL_H
#define ZREGIONANNOTATIONTREEMODEL_H

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>
#include <QIcon>
#include <map>
#include "zregionannotation.h"

namespace nim {

class ZRegionAnnotationTreeModel : public QAbstractItemModel
{
  Q_OBJECT
public:
  enum Column
  {
    AbbreviationColumn, IDColumn, NameColumn, MergeROIColumn, ExportROIColumn, ExportMeshColumn, ColumnCount
  };

  explicit ZRegionAnnotationTreeModel(ZRegionAnnotation &anno, QObject *parent = nullptr);
  ~ZRegionAnnotationTreeModel();

  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
  Qt::ItemFlags flags(const QModelIndex &index) const;
  bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole);

  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const;
  QModelIndex index(int row, int column,
                    const QModelIndex &parent = QModelIndex()) const;
  QModelIndex parent(const QModelIndex &index) const;
  int rowCount(const QModelIndex &parent = QModelIndex()) const;
  int columnCount(const QModelIndex &parent = QModelIndex()) const;

  void clicked(const QModelIndex &index);
  void doubleClicked(const QModelIndex &index);
  void activated(const QModelIndex &index);

protected:

protected:
  ZRegionAnnotation& m_regionAnnotation;
  ZTree<RegionNode>& m_annotationTree;
  std::map<RegionNode*, ZTree<RegionNode>::Iterator> m_nodeToIter;

signals:

public slots:

};

} // namespace nim

#endif // ZREGIONANNOTATIONTREEMODEL_H
