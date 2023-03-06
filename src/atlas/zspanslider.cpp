#include "zspanslider.h"

#include "zlog.h"
#include "zsaturateoperation.h"
#include <QBoxLayout>

namespace nim {

ZSpanSlider::ZSpanSlider(QWidget* parent)
  : QxtSpanSlider(parent)
{}

ZSpanSlider::ZSpanSlider(Qt::Orientation orientation, QWidget* parent)
  : QxtSpanSlider(orientation, parent)
{}

void ZSpanSlider::setLowerValueBlockSignals(int lower)
{
  const QSignalBlocker blocker(this);
  setLowerValue(lower);
}

void ZSpanSlider::setUpperValueBlockSignals(int upper)
{
  const QSignalBlocker blocker(this);
  setUpperValue(upper);
}

ZSpanSliderWithSpinBox::ZSpanSliderWithSpinBox(int lowerValue,
                                               int upperValue,
                                               int min,
                                               int max,
                                               int singleStep,
                                               bool tracking,
                                               QWidget* parent)
  : QWidget(parent)
{
  createWidget(lowerValue, upperValue, min, max, singleStep, tracking);
}

void ZSpanSliderWithSpinBox::setLowerValueBlockSignals(int lower)
{
  m_slider->setLowerValueBlockSignals(lower);
  m_lowerSpinBox->setValueBlockSignals(lower);
}

void ZSpanSliderWithSpinBox::setUpperValueBlockSignals(int upper)
{
  m_slider->setUpperValueBlockSignals(upper);
  m_upperSpinBox->setValueBlockSignals(upper);
}

void ZSpanSliderWithSpinBox::setDataRange(int min, int max)
{
  m_slider->setRange(min, max);
  m_lowerSpinBox->setRange(m_slider->minimum(), m_slider->upperValue());
  m_lowerSpinBox->setValueBlockSignals(m_slider->lowerValue());
  m_upperSpinBox->setRange(m_slider->lowerValue(), m_slider->maximum());
  m_upperSpinBox->setValueBlockSignals(m_slider->upperValue());
}

void ZSpanSliderWithSpinBox::lowerValueChangedFromSlider(int l)
{
  m_lowerSpinBox->setValueBlockSignals(l);
  m_upperSpinBox->setMinimum(l);
  Q_EMIT lowerValueChanged(l);
}

void ZSpanSliderWithSpinBox::upperValueChangedFromSlider(int u)
{
  m_upperSpinBox->setValueBlockSignals(u);
  m_lowerSpinBox->setMaximum(u);
  Q_EMIT upperValueChanged(u);
}

void ZSpanSliderWithSpinBox::valueChangedFromLowerSpinBox(int l)
{
  m_slider->setLowerValueBlockSignals(l);
  m_upperSpinBox->setMinimum(l);
  Q_EMIT lowerValueChanged(l);
}

void ZSpanSliderWithSpinBox::valueChangedFromUpperSpinBox(int u)
{
  m_slider->setUpperValueBlockSignals(u);
  m_lowerSpinBox->setMaximum(u);
  Q_EMIT upperValueChanged(u);
}

void ZSpanSliderWithSpinBox::createWidget(int lowerValue,
                                          int upperValue,
                                          int min,
                                          int max,
                                          int singleStep,
                                          bool tracking)
{
  m_slider = new ZSpanSlider(Qt::Horizontal);
  m_slider->setRange(min, max);
  m_slider->setSpan(lowerValue, upperValue);
  m_slider->setSingleStep(singleStep);
  m_slider->setTracking(tracking);
  m_slider->setHandleMovementMode(QxtSpanSlider::NoOverlapping);
  m_slider->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  m_lowerSpinBox = new ZSpinBox();
  m_lowerSpinBox->setRange(min, upperValue);
  m_lowerSpinBox->setValueBlockSignals(lowerValue);
  m_lowerSpinBox->setSingleStep(singleStep);
  m_upperSpinBox = new ZSpinBox();
  m_upperSpinBox->setRange(lowerValue, max);
  m_upperSpinBox->setValueBlockSignals(upperValue);
  m_upperSpinBox->setSingleStep(singleStep);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_lowerSpinBox);
  lo->addWidget(m_slider);
  lo->addWidget(m_upperSpinBox);
  connect(m_slider, &QxtSpanSlider::lowerValueChanged, this, &ZSpanSliderWithSpinBox::lowerValueChangedFromSlider);
  connect(m_slider, &QxtSpanSlider::upperValueChanged, this, &ZSpanSliderWithSpinBox::upperValueChangedFromSlider);
  connect(m_lowerSpinBox,
          qOverload<int>(&ZSpinBox::valueChanged),
          this,
          &ZSpanSliderWithSpinBox::valueChangedFromLowerSpinBox);
  connect(m_upperSpinBox,
          qOverload<int>(&ZSpinBox::valueChanged),
          this,
          &ZSpanSliderWithSpinBox::valueChangedFromUpperSpinBox);
}

ZDoubleSpanSliderWithSpinBox::ZDoubleSpanSliderWithSpinBox(double lowerValue,
                                                           double upperValue,
                                                           double min,
                                                           double max,
                                                           double singleStep,
                                                           int decimal,
                                                           bool tracking,
                                                           QWidget* parent)
  : QWidget(parent)
  , m_lowerValue(lowerValue)
  , m_upperValue(upperValue)
  , m_min(min)
  , m_max(max)
  , m_step(singleStep)
  , m_decimal(decimal)
  , m_tracking(tracking)
{
  m_sliderMaxValue = 1000000; // roundTo<int>((m_max - m_min) / m_step);
  createWidget();
}

void ZDoubleSpanSliderWithSpinBox::setLowerValueBlockSignals(double lower)
{
  double l = (lower - m_min) / (m_max - m_min) * m_sliderMaxValue;
  m_slider->setLowerValueBlockSignals(l);
  m_lowerSpinBox->setValueBlockSignals(lower);
}

void ZDoubleSpanSliderWithSpinBox::setUpperValueBlockSignals(double upper)
{
  double u = (upper - m_min) / (m_max - m_min) * m_sliderMaxValue;
  m_slider->setUpperValueBlockSignals(u);
  m_upperSpinBox->setValueBlockSignals(upper);
}

void ZDoubleSpanSliderWithSpinBox::setDataRange(double min, double max)
{
  m_min = min;
  m_max = max;
  m_sliderMaxValue = 1e6; // roundTo<int>((m_max - m_min) / m_step);
  m_slider->setRange(0, m_sliderMaxValue);
  double l = m_slider->lowerValue() / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  double u = m_slider->upperValue() / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_lowerSpinBox->setRange(m_min, u);
  m_lowerSpinBox->setValueBlockSignals(l);
  m_upperSpinBox->setRange(l, m_max);
  m_upperSpinBox->setValueBlockSignals(u);
}

void ZDoubleSpanSliderWithSpinBox::lowerValueChangedFromSlider(int l)
{
  m_lowerValue = l / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_lowerSpinBox->setValueBlockSignals(m_lowerValue);
  m_upperSpinBox->setMinimum(m_lowerValue);
  Q_EMIT lowerValueChanged(m_lowerValue);
}

void ZDoubleSpanSliderWithSpinBox::upperValueChangedFromSlider(int u)
{
  m_upperValue = u / static_cast<double>(m_sliderMaxValue) * (m_max - m_min) + m_min;
  m_upperSpinBox->setValueBlockSignals(m_upperValue);
  m_lowerSpinBox->setMaximum(m_upperValue);
  Q_EMIT upperValueChanged(m_upperValue);
}

void ZDoubleSpanSliderWithSpinBox::valueChangedFromLowerSpinBox(double l)
{
  m_lowerValue = l;
  int sliderPos = static_cast<int>((m_lowerValue - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setLowerValueBlockSignals(sliderPos);
  m_upperSpinBox->setMinimum(m_lowerValue);
  Q_EMIT lowerValueChanged(m_lowerValue);
}

void ZDoubleSpanSliderWithSpinBox::valueChangedFromUpperSpinBox(double u)
{
  m_upperValue = u;
  int sliderPos = static_cast<int>((m_upperValue - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setUpperValueBlockSignals(sliderPos);
  m_lowerSpinBox->setMaximum(m_upperValue);
  Q_EMIT upperValueChanged(m_upperValue);
}

void ZDoubleSpanSliderWithSpinBox::createWidget()
{
  m_slider = new ZSpanSlider(Qt::Horizontal);
  m_slider->setRange(0, m_sliderMaxValue);
  m_slider->setSpan(static_cast<int>((m_lowerValue - m_min) / (m_max - m_min) * m_sliderMaxValue),
                    static_cast<int>((m_upperValue - m_min) / (m_max - m_min) * m_sliderMaxValue));
  m_slider->setSingleStep(std::max(1, static_cast<int>(m_step * m_sliderMaxValue / (m_max - m_min))));
  m_slider->setTracking(m_tracking);
  m_slider->setHandleMovementMode(QxtSpanSlider::NoOverlapping);
  m_slider->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
  m_lowerSpinBox = new ZDoubleSpinBox();
  m_lowerSpinBox->setRange(m_min, m_upperValue);
  m_lowerSpinBox->setValueBlockSignals(m_lowerValue);
  m_lowerSpinBox->setSingleStep(m_step);
  m_lowerSpinBox->setDecimals(m_decimal);
  m_upperSpinBox = new ZDoubleSpinBox();
  m_upperSpinBox->setRange(m_lowerValue, m_max);
  m_upperSpinBox->setValueBlockSignals(m_upperValue);
  m_upperSpinBox->setSingleStep(m_step);
  m_upperSpinBox->setDecimals(m_decimal);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_lowerSpinBox);
  lo->addWidget(m_slider);
  lo->addWidget(m_upperSpinBox);
  connect(m_slider,
          &QxtSpanSlider::lowerValueChanged,
          this,
          &ZDoubleSpanSliderWithSpinBox::lowerValueChangedFromSlider);
  connect(m_slider,
          &QxtSpanSlider::upperValueChanged,
          this,
          &ZDoubleSpanSliderWithSpinBox::upperValueChangedFromSlider);
  connect(m_lowerSpinBox,
          qOverload<double>(&ZDoubleSpinBox::valueChanged),
          this,
          &ZDoubleSpanSliderWithSpinBox::valueChangedFromLowerSpinBox);
  connect(m_upperSpinBox,
          qOverload<double>(&ZDoubleSpinBox::valueChanged),
          this,
          &ZDoubleSpanSliderWithSpinBox::valueChangedFromUpperSpinBox);
}

} // namespace nim
