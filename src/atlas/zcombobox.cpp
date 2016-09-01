#include "zcombobox.h"

#include "zlog.h"
#include <QEvent>

namespace nim {

ZComboBox::ZComboBox(QWidget* parent)
  : QComboBox(parent)
{
  installEventFilter(new ZComboBoxEventFilter(this));
  setFocusPolicy(Qt::StrongFocus);
}

QSize ZComboBox::sizeHint() const
{
  QSize size = QComboBox::sizeHint();
  size.setWidth(std::min(size.width(), 160));
  return size;
}

QSize ZComboBox::minimumSizeHint() const
{
  QSize size = QComboBox::minimumSizeHint();
  size.setWidth(std::min(size.width(), 80));
  return size;
}

void ZComboBox::addItemSlot(const QString& text)
{
  //LOG(INFO) << text;
  addItem(text);
}

void ZComboBox::removeItemSlot(const QString& text)
{
  removeItem(findText(text));
}

void ZComboBox::focusInEvent(QFocusEvent* /*e*/)
{
  setFocusPolicy(Qt::WheelFocus);
}

void ZComboBox::focusOutEvent(QFocusEvent* /*e*/)
{
  setFocusPolicy(Qt::StrongFocus);
}

ZComboBoxEventFilter::ZComboBoxEventFilter(QObject* parent)
  : QObject(parent)
{
}

bool ZComboBoxEventFilter::eventFilter(QObject* obj, QEvent* event)
{
  if (event->type() == QEvent::Wheel) {
    if (auto qcb = qobject_cast<QComboBox*>(obj)) {
      if (qcb->focusPolicy() == Qt::WheelFocus) {
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
