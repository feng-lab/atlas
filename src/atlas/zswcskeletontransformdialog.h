#pragma once

#include <QDialog>

class QDoubleSpinBox;
class QRadioButton;

namespace nim {

class ZSwcSkeletonTransformDialog : public QDialog
{
  Q_OBJECT

public:
  enum Axis
  {
    X = 0,
    Y = 1,
    Z = 2
  };

  explicit ZSwcSkeletonTransformDialog(QWidget* parent = nullptr);

  void setTranslateValue(double x, double y, double z);
  void setScaleValue(double x, double y, double z);

  [[nodiscard]] double translateValue(Axis axis) const;
  [[nodiscard]] double scaleValue(Axis axis) const;
  [[nodiscard]] bool isTranslateFirst() const;

private:
  QDoubleSpinBox* m_translate[3]{nullptr, nullptr, nullptr};
  QDoubleSpinBox* m_scale[3]{nullptr, nullptr, nullptr};
  QRadioButton* m_translateFirst = nullptr;
  QRadioButton* m_scaleFirst = nullptr;
};

} // namespace nim
