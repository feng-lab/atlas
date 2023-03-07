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

  void setDataRange(int min, int max);

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
  ZSpanSlider* m_slider;
  ZSpinBox* m_lowerSpinBox;
  ZSpinBox* m_upperSpinBox;
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

  void setDataRange(double min, double max);

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
  ZSpanSlider* m_slider;
  ZDoubleSpinBox* m_lowerSpinBox;
  ZDoubleSpinBox* m_upperSpinBox;
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
