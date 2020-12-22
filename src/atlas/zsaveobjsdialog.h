#pragma once

#include <QDialog>

class QLabel;

class QTreeWidget;

class QDialogButtonBox;

namespace nim {

class ZDoc;

class ZSaveObjsDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZSaveObjsDialog(const ZDoc& doc, const std::vector<size_t>& objs, QWidget* parent = nullptr);

  [[nodiscard]] const std::vector<size_t>& objsToSave() const
  { return m_objsToSave; }

protected:
  void createWidget();

private:
  void collectObjsToSave();

  void discard();

  void updateSaveButton();

  void adjustButtonWidths();

private:
  const ZDoc& m_doc;
  std::vector<size_t> m_objsToSave;

  QLabel* m_label = nullptr;
  QTreeWidget* m_treeWidget = nullptr;
  QDialogButtonBox* m_buttonBox = nullptr;
};

} // namespace nim

