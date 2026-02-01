#pragma once

#include "zspinbox.h"
#include <qxtspanslider.h>

namespace nim {

class ZSpanSlider : public QxtSpanSlider
{
  Q_OBJECT

public:
  using QxtSpanSlider::QxtSpanSlider;

  void setLowerValueBlockSignals(int lower);

  void setUpperValueBlockSignals(int upper);

  void setRangeBlockSignals(int min, int max);
};

class ZSpanSliderWithSpinBox : public QWidget
{
  Q_OBJECT

public:
  explicit ZSpanSliderWithSpinBox(int lowerValue,
                                  int upperValue,
                                  int min,
                                  int max,
                                  int singleStep = 1,
                                  bool tracking = true,
                                  QWidget* parent = nullptr);

  void setLowerValueBlockSignals(int lower);

  void setUpperValueBlockSignals(int upper);

  void setDataRangeBlockSignals(int min, int max);

  [[nodiscard]] ZSpanSlider* slider() const
  {
    return m_slider;
  }
  [[nodiscard]] ZSpinBox* lowerSpinBox() const
  {
    return m_lowerSpinBox;
  }
  [[nodiscard]] ZSpinBox* upperSpinBox() const
  {
    return m_upperSpinBox;
  }

Q_SIGNALS:
  void lowerValueChanged(int lower);

  void upperValueChanged(int upper);

private:
  void createWidget(int lowerValue, int upperValue, int min, int max, int singleStep, bool tracking);

  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(int l);

  void valueChangedFromUpperSpinBox(int u);

private:
  ZSpanSlider* m_slider = nullptr;
  ZSpinBox* m_lowerSpinBox = nullptr;
  ZSpinBox* m_upperSpinBox = nullptr;
};

class ZDoubleSpanSliderWithSpinBox : public QWidget
{
  Q_OBJECT

public:
  explicit ZDoubleSpanSliderWithSpinBox(double lowerValue,
                                        double upperValue,
                                        double min,
                                        double max,
                                        double singleStep = .01,
                                        int decimal = 3,
                                        bool tracking = true,
                                        QWidget* parent = nullptr);

  void setLowerValueBlockSignals(double lower);

  void setUpperValueBlockSignals(double upper);

  void setDataRangeBlockSignals(double min, double max);

  [[nodiscard]] ZSpanSlider* slider() const
  {
    return m_slider;
  }
  [[nodiscard]] ZDoubleSpinBox* lowerSpinBox() const
  {
    return m_lowerSpinBox;
  }
  [[nodiscard]] ZDoubleSpinBox* upperSpinBox() const
  {
    return m_upperSpinBox;
  }

Q_SIGNALS:
  void lowerValueChanged(double lower);

  void upperValueChanged(double upper);

private:
  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(double l);

  void valueChangedFromUpperSpinBox(double u);

  void createWidget();

private:
  ZSpanSlider* m_slider = nullptr;
  ZDoubleSpinBox* m_lowerSpinBox = nullptr;
  ZDoubleSpinBox* m_upperSpinBox = nullptr;
  double m_lowerValue;
  double m_upperValue;
  double m_min;
  double m_max;
  double m_step;
  int m_decimal;
  bool m_tracking;
  int m_sliderMaxValue;
};

} // namespace nim
