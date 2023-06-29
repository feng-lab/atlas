#include "zspinbox.h"

#include "zlog.h"
#include <QEvent>
#include <QSignalBlocker>

namespace nim {

ZSpinBox::ZSpinBox(QWidget* parent)
  : QSpinBox(parent)
{
  installEventFilter(new ZSpinBoxEventFilter(this));
  setFocusPolicy(Qt::StrongFocus);
  setKeyboardTracking(false);
}

QSize ZSpinBox::sizeHint() const
{
  QSize size = QSpinBox::sizeHint();
  size.setWidth(std::min(size.width(), 160));
  return size;
}

QSize ZSpinBox::minimumSizeHint() const
{
  QSize size = QSpinBox::minimumSizeHint();
  size.setWidth(std::min(size.width(), 57));
  return size;
}

void ZSpinBox::setValueBlockSignals(int v)
{
  const QSignalBlocker blocker(this);
  setValue(v);
}

void ZSpinBox::setRangeBlockSignals(int min, int max)
{
  const QSignalBlocker blocker(this);
  setRange(min, max);
}

void ZSpinBox::focusInEvent(QFocusEvent* e)
{
  QSpinBox::focusInEvent(e);
  setFocusPolicy(Qt::WheelFocus);
}

void ZSpinBox::focusOutEvent(QFocusEvent* e)
{
  QSpinBox::focusOutEvent(e);
  setFocusPolicy(Qt::StrongFocus);
}

ZDoubleSpinBox::ZDoubleSpinBox(QWidget* parent)
  : QDoubleSpinBox(parent)
{
  installEventFilter(new ZSpinBoxEventFilter(this));
  setFocusPolicy(Qt::StrongFocus);
  setKeyboardTracking(false);
}

QSize ZDoubleSpinBox::sizeHint() const
{
  QSize size = QDoubleSpinBox::sizeHint();
  size.setWidth(std::min(size.width(), 160));
  return size;
}

QSize ZDoubleSpinBox::minimumSizeHint() const
{
  QSize size = QDoubleSpinBox::minimumSizeHint();
  size.setWidth(std::min(size.width(), 57));
  return size;
}

void ZDoubleSpinBox::setValueBlockSignals(double v)
{
  const QSignalBlocker blocker(this);
  setValue(v);
}

void ZDoubleSpinBox::setRangeBlockSignals(double min, double max)
{
  const QSignalBlocker blocker(this);
  setRange(min, max);
}

void ZDoubleSpinBox::focusInEvent(QFocusEvent* e)
{
  QDoubleSpinBox::focusInEvent(e);
  setFocusPolicy(Qt::WheelFocus);
}

void ZDoubleSpinBox::focusOutEvent(QFocusEvent* e)
{
  QDoubleSpinBox::focusOutEvent(e);
  setFocusPolicy(Qt::StrongFocus);
}

ZSpinBoxEventFilter::ZSpinBoxEventFilter(QObject* parent)
  : QObject(parent)
{}

bool ZSpinBoxEventFilter::eventFilter(QObject* obj, QEvent* event)
{
  if (event->type() == QEvent::Wheel) {
    if (auto qasb = qobject_cast<QAbstractSpinBox*>(obj)) {
      if (qasb->focusPolicy() == Qt::WheelFocus) {
        event->accept();
        return false;
      }
      event->ignore();
      return true;
    }
  }
  return QObject::eventFilter(obj, event);
}

} // namespace nim
