#pragma once

#include <QDialog>

class QButtonGroup;
class QRadioButton;
class QSpinBox;

namespace nim {

class ZSwcTypeDialog final : public QDialog
{
  Q_OBJECT

public:
  enum class SelectionMode
  {
    SwcNode,
    WholeTree
  };

  enum class PickingMode
  {
    Individual,
    Downstream,
    Connection,
    MainTrunk,
    TrunkLevel,
    Traffic,
    BranchLevel,
    LongestLeaf,
    FurthestLeaf,
    Root,
    Subtree
  };

  explicit ZSwcTypeDialog(SelectionMode selectionMode, QWidget* parent = nullptr);

  [[nodiscard]] int type() const;
  [[nodiscard]] PickingMode pickingMode() const;

private:
  void applySelectionModeVisibility(SelectionMode selectionMode);

  QSpinBox* m_typeSpinBox = nullptr;
  QButtonGroup* m_group = nullptr;

  QRadioButton* m_individual = nullptr;
  QRadioButton* m_downstream = nullptr;
  QRadioButton* m_connection = nullptr;
  QRadioButton* m_mainTrunk = nullptr;
  QRadioButton* m_trunkLevel = nullptr;
  QRadioButton* m_branchLevel = nullptr;
  QRadioButton* m_traffic = nullptr;
  QRadioButton* m_longestLeaf = nullptr;
  QRadioButton* m_furthestLeaf = nullptr;
  QRadioButton* m_root = nullptr;
  QRadioButton* m_subtree = nullptr;
};

} // namespace nim
