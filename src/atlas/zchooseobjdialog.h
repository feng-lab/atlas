#pragma once

#include <QDialog>
#include <QList>

class QLabel;

class QTreeWidget;

class QDialogButtonBox;

namespace nim {

class ZObjDoc;

class ZChooseObjDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZChooseObjDialog(const ZObjDoc& doc, QWidget* parent = 0);

  size_t selectedID() const
  { return m_selectedID; }

protected:
  void createWidget();

private:
  void updateSelectedID();

private:
  const ZObjDoc& m_doc;
  size_t m_selectedID;

  QLabel* m_label;
  QTreeWidget* m_treeWidget;
  QDialogButtonBox* m_buttonBox;
};

} // namespace nim

