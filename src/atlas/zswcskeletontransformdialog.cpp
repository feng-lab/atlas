#include "zswcskeletontransformdialog.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

namespace nim {

namespace {

QDoubleSpinBox* makeTranslateSpinBox(QWidget* parent)
{
  auto* box = new QDoubleSpinBox(parent);
  // Match neuTube SwcSkeletonTransformDialog defaults.
  box->setRange(-1000000.0, 1000000.0);
  box->setSingleStep(1.0);
  box->setValue(0.0);
  return box;
}

QDoubleSpinBox* makeScaleSpinBox(QWidget* parent)
{
  auto* box = new QDoubleSpinBox(parent);
  // Match neuTube SwcSkeletonTransformDialog defaults.
  box->setRange(0.01, 1000000.0);
  box->setDecimals(5);
  box->setSingleStep(0.1);
  box->setValue(1.0);
  return box;
}

} // namespace

ZSwcSkeletonTransformDialog::ZSwcSkeletonTransformDialog(QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Transform SWC"));

  auto* layout = new QVBoxLayout(this);

  auto* grid = new QGridLayout();
  grid->addWidget(new QLabel(QString(), this), 0, 0);
  grid->addWidget(new QLabel(tr("x"), this), 0, 1);
  grid->addWidget(new QLabel(tr("y"), this), 0, 2);
  grid->addWidget(new QLabel(tr("z"), this), 0, 3);

  grid->addWidget(new QLabel(tr("Translate"), this), 1, 0);
  m_translate[X] = makeTranslateSpinBox(this);
  m_translate[Y] = makeTranslateSpinBox(this);
  m_translate[Z] = makeTranslateSpinBox(this);
  grid->addWidget(m_translate[X], 1, 1);
  grid->addWidget(m_translate[Y], 1, 2);
  grid->addWidget(m_translate[Z], 1, 3);

  grid->addWidget(new QLabel(tr("Scale"), this), 2, 0);
  m_scale[X] = makeScaleSpinBox(this);
  m_scale[Y] = makeScaleSpinBox(this);
  m_scale[Z] = makeScaleSpinBox(this);
  grid->addWidget(m_scale[X], 2, 1);
  grid->addWidget(m_scale[Y], 2, 2);
  grid->addWidget(m_scale[Z], 2, 3);

  layout->addLayout(grid);

  m_translateFirst = new QRadioButton(tr("Translate first"), this);
  m_scaleFirst = new QRadioButton(tr("Scale first"), this);
  m_translateFirst->setChecked(true);
  auto* group = new QButtonGroup(this);
  group->addButton(m_translateFirst);
  group->addButton(m_scaleFirst);

  auto* orderRow = new QHBoxLayout();
  orderRow->addWidget(m_translateFirst);
  orderRow->addWidget(m_scaleFirst);
  orderRow->addStretch(1);
  layout->addLayout(orderRow);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void ZSwcSkeletonTransformDialog::setTranslateValue(double x, double y, double z)
{
  m_translate[X]->setValue(x);
  m_translate[Y]->setValue(y);
  m_translate[Z]->setValue(z);
}

void ZSwcSkeletonTransformDialog::setScaleValue(double x, double y, double z)
{
  m_scale[X]->setValue(x);
  m_scale[Y]->setValue(y);
  m_scale[Z]->setValue(z);
}

double ZSwcSkeletonTransformDialog::translateValue(Axis axis) const
{
  return m_translate[axis]->value();
}

double ZSwcSkeletonTransformDialog::scaleValue(Axis axis) const
{
  return m_scale[axis]->value();
}

bool ZSwcSkeletonTransformDialog::isTranslateFirst() const
{
  return m_translateFirst->isChecked();
}

} // namespace nim
