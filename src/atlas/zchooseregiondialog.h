#pragma once

#include <QDialog>

class QLabel;

class QTreeWidget;

class QDialogButtonBox;

namespace nim {

class ZRegionAnnotation;

class ZChooseRegionDialog : public QDialog
{
Q_OBJECT
public:
  explicit ZChooseRegionDialog(const ZRegionAnnotation& ra, QWidget* parent = nullptr);

  [[nodiscard]] size_t selectedID() const
  { return m_selectedID; }

protected:
  void createWidget();

private:
  void updateSelectedID();

private:
  const ZRegionAnnotation& m_regionAnnotation;
  size_t m_selectedID = 0;

  QLabel* m_label = nullptr;
  QTreeWidget* m_treeWidget = nullptr;
  QDialogButtonBox* m_buttonBox = nullptr;
};

} // namespace nim



