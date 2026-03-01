#include "zresolutiondialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace nim {

namespace {

QDoubleSpinBox* makeScaleSpinBox(QWidget* parent)
{
  auto* box = new QDoubleSpinBox(parent);
  box->setRange(0.0, 1000000.0);
  box->setSingleStep(0.1);
  box->setValue(1.0);
  return box;
}

} // namespace

ZResolutionDialog::ZResolutionDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Scaling Factors"));

  auto* layout = new QVBoxLayout(this);

  m_xScale = makeScaleSpinBox(this);
  m_yScale = makeScaleSpinBox(this);
  m_zScale = makeScaleSpinBox(this);

  {
    auto* header = new QHBoxLayout();
    header->addWidget(new QLabel(tr("Scale"), this));
    header->addStretch(1);
    m_sameXY = new QCheckBox(tr("Same XY"), this);
    m_sameXY->setChecked(true);
    header->addWidget(m_sameXY);
    layout->addLayout(header);
  }

  auto addRow = [&](const QString& axis, QDoubleSpinBox* box) {
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel(axis, this));
    row->addWidget(box);
    row->addStretch(1);
    layout->addLayout(row);
  };

  addRow(tr("x"), m_xScale);
  addRow(tr("y"), m_yScale);
  addRow(tr("z"), m_zScale);

  connect(m_sameXY, &QCheckBox::toggled, this, &ZResolutionDialog::linkXY);
  linkXY(m_sameXY->isChecked());

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

double ZResolutionDialog::xScale() const
{
  return m_xScale->value();
}

double ZResolutionDialog::yScale() const
{
  return m_yScale->value();
}

double ZResolutionDialog::zScale() const
{
  return m_zScale->value();
}

void ZResolutionDialog::setXScale(double v)
{
  m_xScale->setValue(v);
}

void ZResolutionDialog::setYScale(double v)
{
  m_yScale->setValue(v);
}

void ZResolutionDialog::setZScale(double v)
{
  m_zScale->setValue(v);
}

void ZResolutionDialog::setXScaleQuietly(double v)
{
  const QSignalBlocker blocker(m_xScale);
  m_xScale->setValue(v);
}

void ZResolutionDialog::setYScaleQuietly(double v)
{
  const QSignalBlocker blocker(m_yScale);
  m_yScale->setValue(v);
}

void ZResolutionDialog::linkXY(bool linked)
{
  if (linked) {
    connect(m_xScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ZResolutionDialog::setYScaleQuietly);
    connect(m_yScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ZResolutionDialog::setXScaleQuietly);
  } else {
    disconnect(m_xScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ZResolutionDialog::setYScaleQuietly);
    disconnect(m_yScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &ZResolutionDialog::setXScaleQuietly);
  }
}

} // namespace nim
