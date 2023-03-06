#pragma once

#include <QCheckBox>

namespace nim {

class ZCheckBox : public QCheckBox
{
  Q_OBJECT

public:
  explicit ZCheckBox(QWidget* parent = nullptr);

  void setCheckedBlockSignals(bool v);
};

} // namespace nim
