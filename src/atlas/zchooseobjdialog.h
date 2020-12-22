#pragma once

#include "zlog.h"
#include <QDialog>

class QLabel;

class QTreeWidget;

class QDialogButtonBox;

namespace nim {

class ZObjDoc;

class ZDoc;

class ZChooseObjDialog : public QDialog
{
Q_OBJECT
public:
  ZChooseObjDialog(const ZObjDoc& objDoc, bool multipleSelection, QWidget* parent = nullptr);

  ZChooseObjDialog(const ZDoc& doc, bool multipleSelection, QWidget* parent = nullptr);

  [[nodiscard]] size_t selectedID() const
  { CHECK(!m_multipleSelection); return m_selectedIDs.empty() ? 0 : m_selectedIDs[0]; }

  [[nodiscard]] const std::vector<size_t>& selectedIDs() const
  { CHECK(m_multipleSelection); return m_selectedIDs; }

protected:
  void createWidget();

private:
  void updateSelectedIDs();

private:
  const ZObjDoc* m_objDoc = nullptr;
  const ZDoc* m_doc = nullptr;
  bool m_multipleSelection = false;

  std::vector<size_t> m_selectedIDs;

  QLabel* m_label = nullptr;
  QTreeWidget* m_treeWidget = nullptr;
  QDialogButtonBox* m_buttonBox = nullptr;
};

} // namespace nim

