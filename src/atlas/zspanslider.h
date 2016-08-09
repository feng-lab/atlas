#pragma once

#include "zspinbox.h"

class QxtSpanSlider;

namespace nim {

class ZSpanSliderWithSpinBox : public QWidget
{
Q_OBJECT
public:
  explicit ZSpanSliderWithSpinBox(int lowerValue, int upperValue, int min, int max, int singleStep = 1,
                                  bool tracking = true, QWidget* parent = 0);

  void setLowerValue(int lower);

  void setUpperValue(int upper);

  void setDataRange(int min, int max);

signals:

  void lowerValueChanged(int lower);

  void upperValueChanged(int upper);

private:
  void createWidget(int lowerValue, int upperValue, int min, int max, int singleStep,
                    bool tracking);

  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(int l);

  void valueChangedFromUpperSpinBox(int u);

private:
  QxtSpanSlider* m_slider;
  ZSpinBox* m_lowerSpinBox;
  ZSpinBox* m_upperSpinBox;
};

class ZDoubleSpanSliderWithSpinBox : public QWidget
{
Q_OBJECT
public:
  explicit ZDoubleSpanSliderWithSpinBox(double lowerValue, double upperValue, double min, double max,
                                        double singleStep = .01,
                                        int decimal = 3, bool tracking = true, QWidget* parent = 0);

  void setLowerValue(double lower);

  void setUpperValue(double upper);

  void setDataRange(double min, double max);

signals:

  void lowerValueChanged(double lower);

  void upperValueChanged(double upper);

private:
  void lowerValueChangedFromSlider(int l);

  void upperValueChangedFromSlider(int u);

  void valueChangedFromLowerSpinBox(double l);

  void valueChangedFromUpperSpinBox(double u);

  void createWidget();

private:
  QxtSpanSlider* m_slider;
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

