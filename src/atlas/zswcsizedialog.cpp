#include "zswcsizedialog.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace nim {

namespace {

QDoubleSpinBox* makeAddSpinBox(QWidget* parent)
{
  auto* box = new QDoubleSpinBox(parent);
  // Match neuTube SwcSizeDialog defaults.
  box->setRange(0.0, 1000000.0);
  box->setSingleStep(0.1);
  box->setValue(0.0);
  return box;
}

QDoubleSpinBox* makeMulSpinBox(QWidget* parent)
{
  auto* box = new QDoubleSpinBox(parent);
  // Match neuTube SwcSizeDialog defaults.
  box->setRange(0.0, 1000000.0);
  box->setDecimals(5);
  box->setSingleStep(0.1);
  box->setValue(1.0);
  return box;
}

} // namespace

ZSwcSizeDialog::ZSwcSizeDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Change SWC Size"));

  auto* layout = new QVBoxLayout(this);

  m_addValue = makeAddSpinBox(this);
  m_mulValue = makeMulSpinBox(this);

  auto* row = new QHBoxLayout();
  row->addWidget(new QLabel(tr("New radius"), this));
  row->addWidget(new QLabel(tr("="), this));
  row->addWidget(new QLabel(tr("current radius"), this));
  row->addWidget(new QLabel(tr("x"), this));
  row->addWidget(m_mulValue);
  row->addWidget(new QLabel(tr("+"), this));
  row->addWidget(m_addValue);
  layout->addLayout(row);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

double ZSwcSizeDialog::addValue() const
{
  return m_addValue->value();
}

double ZSwcSizeDialog::mulValue() const
{
  return m_mulValue->value();
}

void ZSwcSizeDialog::setAddValue(double v)
{
  m_addValue->setValue(v);
}

void ZSwcSizeDialog::setMulValue(double v)
{
  m_mulValue->setValue(v);
}

} // namespace nim
