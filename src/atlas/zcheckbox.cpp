#include "zcheckbox.h"

namespace nim {

ZCheckBox::ZCheckBox(QWidget* parent)
  : QCheckBox(parent)
{}

void ZCheckBox::setCheckedBlockSignals(bool v)
{
  const QSignalBlocker blocker(this);
  setChecked(v);
}

} // namespace nim
