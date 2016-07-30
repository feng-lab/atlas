#ifndef ZITEMSELECTIONMODEL_H
#define ZITEMSELECTIONMODEL_H

#include <QItemSelectionModel>

namespace nim {

class ZObjModel;

class ZObjDoc;

class ZItemSelectionModel : public QItemSelectionModel
{
Q_OBJECT
public:
  explicit ZItemSelectionModel(ZObjModel* model, QObject* parent = 0);

  size_t numSelectedObjs() const;

  QList<size_t> selectedObjs() const;

  QList<size_t> selectedObjsOfDoc(const ZObjDoc* objD) const;

  bool isObjSelected(size_t id) const;

  void setObjSelected(size_t id, bool v);

  void deselectObj(size_t id);

  void clearAndSelectObj(size_t id);

  void appendSelectObj(size_t id);

private:
  void convertSelectionChangedSignal(const QItemSelection& selected, const QItemSelection& deselected);

private:
  ZObjModel* m_model;
};

} // namespace nim

#endif // ZITEMSELECTIONMODEL_H
