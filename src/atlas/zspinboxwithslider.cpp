#include "zspinboxwithslider.h"

#include "zspinbox.h"
#include "zlog.h"
#include <QHBoxLayout>
#include <QEvent>
#include <limits>

namespace nim {

ZSliderEventFilter::ZSliderEventFilter(QObject* parent)
  : QObject(parent)
{}

bool ZSliderEventFilter::eventFilter(QObject* obj, QEvent* event)
{
  if (event->type() == QEvent::Wheel) {
    if (auto qas = qobject_cast<QAbstractSlider*>(obj)) {
      if (qas->focusPolicy() == Qt::WheelFocus) {
        event->accept();
        return false;
      }
      event->ignore();
      return true;
    }
  }
  return QObject::eventFilter(obj, event);
}

ZSlider2::ZSlider2(QWidget* parent)
  : QSlider(Qt::Horizontal, parent)
{
  installEventFilter(new ZSliderEventFilter(this));
  setFocusPolicy(Qt::StrongFocus);
}

ZSlider2::ZSlider2(Qt::Orientation ori, QWidget* parent)
  : QSlider(ori, parent)
{}

void ZSlider2::setValueBlockSignals(int v)
{
  const QSignalBlocker blocker(this);
  setValue(v);
}

void ZSlider2::focusInEvent(QFocusEvent* e)
{
  QSlider::focusInEvent(e);
  setFocusPolicy(Qt::WheelFocus);
}

void ZSlider2::focusOutEvent(QFocusEvent* e)
{
  QSlider::focusOutEvent(e);
  setFocusPolicy(Qt::StrongFocus);
}

ZSpinBoxWithSlider::ZSpinBoxWithSlider(int value,
                                       int min,
                                       int max,
                                       int step,
                                       bool tracking,
                                       const QString& prefix,
                                       const QString& suffix,
                                       QWidget* parent)
  : QWidget(parent)
{
  createWidget(value, min, max, step, tracking, prefix, suffix);
}

void ZSpinBoxWithSlider::setValueBlockSignals(int v)
{
  m_slider->setValueBlockSignals(v);
  m_spinBox->setValueBlockSignals(v);
}

void ZSpinBoxWithSlider::valueChangedFromSlider(int v)
{
  m_spinBox->setValueBlockSignals(v);
  Q_EMIT valueChanged(v);
}

void ZSpinBoxWithSlider::valueChangedFromSpinBox(int v)
{
  m_slider->setValueBlockSignals(v);
  Q_EMIT valueChanged(v);
}

void ZSpinBoxWithSlider::setDataRange(int min, int max)
{
  m_slider->setRange(min, max);
  m_spinBox->setRange(min, max);
}

void ZSpinBoxWithSlider::createWidget(int value,
                                      int min,
                                      int max,
                                      int step,
                                      bool tracking,
                                      const QString& prefix,
                                      const QString& suffix)
{
  m_slider = new ZSlider2();
  m_slider->setRange(min, max);
  m_slider->setValue(value);
  m_slider->setSingleStep(step);
  m_slider->setTracking(tracking);
  m_spinBox = new ZSpinBox();
  m_spinBox->setRange(min, max);
  m_spinBox->setValue(value);
  m_spinBox->setSingleStep(step);
  m_spinBox->setPrefix(prefix);
  m_spinBox->setSuffix(suffix);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_spinBox);
  lo->addWidget(m_slider);
  connect(m_slider, &ZSlider2::valueChanged, this, &ZSpinBoxWithSlider::valueChangedFromSlider);
  connect(m_spinBox, qOverload<int>(&ZSpinBox::valueChanged), this, &ZSpinBoxWithSlider::valueChangedFromSpinBox);
}

ZDoubleSpinBoxWithSlider::ZDoubleSpinBoxWithSlider(double value,
                                                   double min,
                                                   double max,
                                                   double step,
                                                   int decimal,
                                                   bool tracking,
                                                   const QString& prefix,
                                                   const QString& suffix,
                                                   QWidget* parent)
  : QWidget(parent)
  , m_value(value)
  , m_min(min)
  , m_max(max)
  , m_step(step)
  , m_decimal(decimal)
  , m_tracking(tracking)
{
  double sliderMaxValue = (m_max - m_min) / m_step;
  if (sliderMaxValue > std::numeric_limits<int>::max()) {
    m_sliderMaxValue = std::numeric_limits<int>::max();
  } else {
    m_sliderMaxValue = static_cast<int>(sliderMaxValue);
  }
  createWidget(prefix, suffix);
}

void ZDoubleSpinBoxWithSlider::setValueBlockSignals(double v)
{
  m_spinBox->setValueBlockSignals(v);
  int sliderPos = static_cast<int>((v - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setValueBlockSignals(sliderPos);
}

void ZDoubleSpinBoxWithSlider::valueChangedFromSlider(int v)
{
  m_value = static_cast<double>(v) / m_sliderMaxValue * (m_max - m_min) + m_min;
  m_spinBox->setValueBlockSignals(m_value);
  Q_EMIT valueChanged(m_value);
}

void ZDoubleSpinBoxWithSlider::valueChangedFromSpinBox(double v)
{
  m_value = v;
  int sliderPos = static_cast<int>((m_value - m_min) / (m_max - m_min) * m_sliderMaxValue);
  m_slider->setValueBlockSignals(sliderPos);
  Q_EMIT valueChanged(m_value);
}

void ZDoubleSpinBoxWithSlider::setDataRange(double min, double max)
{
  m_min = min;
  m_max = max;
  double sliderMaxValue = (m_max - m_min) / m_step;
  if (sliderMaxValue > std::numeric_limits<int>::max()) {
    m_sliderMaxValue = std::numeric_limits<int>::max();
  } else {
    m_sliderMaxValue = static_cast<int>(sliderMaxValue);
  }
  m_slider->setRange(0, m_sliderMaxValue);
  m_spinBox->setRange(m_min, m_max);
}

void ZDoubleSpinBoxWithSlider::createWidget(const QString& prefix, const QString& suffix)
{
  m_slider = new ZSlider2();
  m_slider->setRange(0, m_sliderMaxValue);
  m_slider->setValue(static_cast<int>((m_value - m_min) / (m_max - m_min) * m_sliderMaxValue));
  m_slider->setSingleStep(std::max(1, static_cast<int>(m_step * m_sliderMaxValue / (m_max - m_min))));
  m_slider->setTracking(m_tracking);
  m_spinBox = new ZDoubleSpinBox();
  m_spinBox->setRange(m_min, m_max);
  m_spinBox->setValue(m_value);
  m_spinBox->setSingleStep(m_step);
  m_spinBox->setDecimals(m_decimal);
  m_spinBox->setPrefix(prefix);
  m_spinBox->setSuffix(suffix);
  auto lo = new QHBoxLayout(this);
  lo->setContentsMargins(0, 0, 0, 0);
  lo->addWidget(m_spinBox);
  lo->addWidget(m_slider);
  connect(m_slider, &ZSlider2::valueChanged, this, &ZDoubleSpinBoxWithSlider::valueChangedFromSlider);
  connect(m_spinBox,
          qOverload<double>(&ZDoubleSpinBox::valueChanged),
          this,
          &ZDoubleSpinBoxWithSlider::valueChangedFromSpinBox);
}

} // namespace nim
