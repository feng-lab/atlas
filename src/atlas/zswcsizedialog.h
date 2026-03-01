#pragma once

#include <QDialog>

class QDoubleSpinBox;

namespace nim {

class ZSwcSizeDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZSwcSizeDialog(QWidget* parent = nullptr);

  [[nodiscard]] double addValue() const;
  [[nodiscard]] double mulValue() const;

  void setAddValue(double v);
  void setMulValue(double v);

private:
  QDoubleSpinBox* m_addValue = nullptr;
  QDoubleSpinBox* m_mulValue = nullptr;
};

} // namespace nim
