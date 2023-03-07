#pragma once

#include <QCheckBox>

namespace nim {

class ZCheckBox : public QCheckBox
{
  Q_OBJECT

public:
  using QCheckBox::QCheckBox;

  void setCheckedBlockSignals(bool v);
};

} // namespace nim
