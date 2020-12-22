#pragma once

#include <QItemSelectionModel>

namespace nim {

class ZObjModel;

class ZObjDoc;

class ZItemSelectionModel : public QItemSelectionModel
{
Q_OBJECT
public:
  explicit ZItemSelectionModel(ZObjModel* model, QObject* parent = nullptr);

  [[nodiscard]] size_t numSelectedObjs() const;

  [[nodiscard]] std::vector<size_t> selectedObjs() const;

  std::vector<size_t> selectedObjsOfDoc(const ZObjDoc* objD) const;

  [[nodiscard]] bool isObjSelected(size_t id) const;

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

