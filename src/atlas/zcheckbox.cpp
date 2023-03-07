#include "zcheckbox.h"

namespace nim {

void ZCheckBox::setCheckedBlockSignals(bool v)
{
  const QSignalBlocker blocker(this);
  setChecked(v);
}

} // namespace nim
