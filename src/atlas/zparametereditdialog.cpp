#include "zparametereditdialog.h"

#include <QVBoxLayout>
#include <QDialogButtonBox>

namespace nim {

ZParameterEditDialog::ZParameterEditDialog(ZParameter& para, QWidget* parent)
  : QDialog(parent)
  , m_para(para)
{
  auto lo = new QGridLayout;
  addWidget(m_para.createNameLabel(this), m_para.createWidget(this), lo);

  auto* vlo = new QVBoxLayout(this);
  vlo->addLayout(lo);
  auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  vlo->addWidget(buttonBox);

  connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  setWindowTitle(para.name());
}

void ZParameterEditDialog::raiseAndActivate()
{
  showNormal();
  raise();
  activateWindow();
}

void ZParameterEditDialog::addWidget(QLabel* label, QWidget* wg, QGridLayout* lo)
{
  // QHBoxLayout *hbl = new QHBoxLayout;
  label->setMinimumWidth(125);
  label->setWordWrap(true);
  // hbl->addWidget(label);
  wg->setMinimumWidth(175);
  wg->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  // hbl->addWidget(wg);
  // return hbl;
  int row = lo->rowCount() + 1;
  lo->addWidget(label, row, 0);
  lo->addWidget(wg, row, 1);
}

} // namespace nim
