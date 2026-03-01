#include "zswctypedialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace nim {

ZSwcTypeDialog::ZSwcTypeDialog(SelectionMode selectionMode, QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Change Swc Type"));

  auto* root = new QVBoxLayout(this);

  {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(tr("Type"), this));
    m_typeSpinBox = new QSpinBox(this);
    m_typeSpinBox->setRange(0, 65535);
    row->addWidget(m_typeSpinBox);
    root->addLayout(row);
  }

  m_group = new QButtonGroup(this);

  auto addRadio = [&](const QString& label, QRadioButton*& out) {
    out = new QRadioButton(label, this);
    root->addWidget(out);
    m_group->addButton(out);
  };

  addRadio(tr("Individual"), m_individual);
  addRadio(tr("Downstream"), m_downstream);
  addRadio(tr("Connection"), m_connection);
  addRadio(tr("Main trunk"), m_mainTrunk);
  addRadio(tr("Trunk level"), m_trunkLevel);
  addRadio(tr("Branch level"), m_branchLevel);
  addRadio(tr("Traffic"), m_traffic);
  addRadio(tr("Longest leaf"), m_longestLeaf);
  addRadio(tr("Furthest leaf"), m_furthestLeaf);
  addRadio(tr("Root"), m_root);
  addRadio(tr("Subtree"), m_subtree);

  m_individual->setChecked(true);
  applySelectionModeVisibility(selectionMode);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

int ZSwcTypeDialog::type() const
{
  return m_typeSpinBox->value();
}

ZSwcTypeDialog::PickingMode ZSwcTypeDialog::pickingMode() const
{
  PickingMode mode = PickingMode::Individual;

  if (m_individual->isChecked()) {
    mode = PickingMode::Individual;
  }
  if (m_connection->isChecked()) {
    mode = PickingMode::Connection;
  }
  if (m_downstream->isChecked()) {
    mode = PickingMode::Downstream;
  }
  if (m_mainTrunk->isChecked()) {
    mode = PickingMode::MainTrunk;
  }
  if (m_traffic->isChecked()) {
    mode = PickingMode::Traffic;
  }
  if (m_longestLeaf->isChecked()) {
    mode = PickingMode::LongestLeaf;
  }
  if (m_furthestLeaf->isChecked()) {
    mode = PickingMode::FurthestLeaf;
  }
  if (m_trunkLevel->isChecked()) {
    mode = PickingMode::TrunkLevel;
  }
  if (m_root->isChecked()) {
    mode = PickingMode::Root;
  }
  if (m_branchLevel->isChecked()) {
    mode = PickingMode::BranchLevel;
  }
  if (m_subtree->isChecked()) {
    mode = PickingMode::Subtree;
  }

  return mode;
}

void ZSwcTypeDialog::applySelectionModeVisibility(SelectionMode selectionMode)
{
  switch (selectionMode) {
    case SelectionMode::WholeTree:
      m_mainTrunk->show();
      m_connection->hide();
      m_downstream->hide();
      m_traffic->show();
      m_trunkLevel->show();
      m_root->show();
      m_subtree->show();
      break;
    case SelectionMode::SwcNode:
      m_mainTrunk->hide();
      m_connection->show();
      m_downstream->show();
      m_traffic->hide();
      m_trunkLevel->hide();
      m_root->hide();
      m_subtree->hide();
      break;
  }
}

} // namespace nim
