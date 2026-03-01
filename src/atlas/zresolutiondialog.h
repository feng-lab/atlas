#pragma once

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;

namespace nim {

class ZResolutionDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ZResolutionDialog(QWidget* parent = nullptr);

  [[nodiscard]] double xScale() const;
  [[nodiscard]] double yScale() const;
  [[nodiscard]] double zScale() const;

  void setXScale(double v);
  void setYScale(double v);
  void setZScale(double v);

private:
  void linkXY(bool linked);
  void setXScaleQuietly(double v);
  void setYScaleQuietly(double v);

  QDoubleSpinBox* m_xScale = nullptr;
  QDoubleSpinBox* m_yScale = nullptr;
  QDoubleSpinBox* m_zScale = nullptr;
  QCheckBox* m_sameXY = nullptr;
};

} // namespace nim
