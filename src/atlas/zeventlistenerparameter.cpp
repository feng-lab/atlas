#include "zeventlistenerparameter.h"
#include <QLabel>
#include "zlog.h"

namespace nim {

ZEventListenerParameter::ZEventListenerParameter(const QString &name, bool sharing, QObject *parent)
  : ZParameter(name, parent)
  , m_sharing(sharing)
{
}

void ZEventListenerParameter::listenTo(const QString &actionName, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers, QEvent::Type type)
{
  m_mouseEvents.push_back(MouseEvent(actionName, buttons, modifiers, type));
  emit valueChanged();
}

void ZEventListenerParameter::listenTo(const QString &actionName, Qt::Key key, Qt::KeyboardModifiers modifiers, QEvent::Type type)
{
  m_keyEvents.push_back(KeyEvent(actionName, key, modifiers, type));
  emit valueChanged();
}

void ZEventListenerParameter::clearAll()
{
  m_mouseEvents.clear();
  m_keyEvents.clear();
  emit valueChanged();
}

void ZEventListenerParameter::sendEvent(QEvent *e, int w, int h)
{
  if (!m_isWidgetsEnabled)
    return;

  if (QMouseEvent* mouseEvent = dynamic_cast<QMouseEvent*>(e)) {
    bool accept = false;
    //LINFO() << mouseEvent->modifiers() << mouseEvent->button() << mouseEvent->buttons();
    for (int i=0; i<m_mouseEvents.size(); i++) {
      accept = true;
      accept &= (mouseEvent->modifiers() == m_mouseEvents[i].modifiers);
      accept &= (mouseEvent->type() == m_mouseEvents[i].type);
      if (mouseEvent->type() == QEvent::MouseMove)
        accept &= ((mouseEvent->buttons() == m_mouseEvents[i].buttons));
      else
        accept &= ((mouseEvent->button() == m_mouseEvents[i].buttons));
      if (accept)
        break;
    }
    if (accept) {
      emit eventTriggered(e, w, h);
      emit mouseEventTriggered(mouseEvent, w, h);

      if (m_sharing)
        e->ignore();
    }
  } else if (QWheelEvent *wheelEvent = dynamic_cast<QWheelEvent*>(e)) {
    bool accept = false;
    for (int i=0; i<m_mouseEvents.size(); i++) {
      accept = true;
      accept &= (wheelEvent->modifiers() == m_mouseEvents[i].modifiers);
      accept &= (wheelEvent->type() == m_mouseEvents[i].type);
      accept &= ((wheelEvent->buttons() == m_mouseEvents[i].buttons));
      if (accept)
        break;
    }
    if (accept) {
      emit eventTriggered(e, w, h);
      emit wheelEventTriggered(wheelEvent, w, h);

      if (m_sharing)
        e->ignore();
    }
  } else if (QKeyEvent* keyEvent = dynamic_cast<QKeyEvent*>(e)) {
    bool accept = false;
    for (int i=0; i<m_keyEvents.size(); i++) {
      accept = true;
      accept &= (keyEvent->modifiers() == m_keyEvents[i].modifiers);
      accept &= (keyEvent->type() == m_keyEvents[i].type);
      accept &= (keyEvent->key() == m_keyEvents[i].key);
      if (accept)
        break;
    }
    if (accept) {
      emit eventTriggered(e, w, h);
      emit keyEventTriggered(keyEvent, w, h);

      if (m_sharing)
        e->ignore();
    }
  }
}

QWidget *ZEventListenerParameter::actualCreateWidget(QWidget *parent)
{
  // TODO
  return new QLabel("Place holder", parent);
}

void ZEventListenerParameter::setSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
  const ZEventListenerParameter* src = static_cast<const ZEventListenerParameter*>(&rhs);
  m_sharing = src->m_sharing;
  m_mouseEvents = src->m_mouseEvents;
  m_keyEvents = src->m_keyEvents;
  ZParameter::setSameAs(rhs);
}

void ZEventListenerParameter::setValueSameAs(const ZParameter &rhs)
{
  assert(this->isSameType(rhs));
}

void ZEventListenerParameter::interpolate(const ZParameter &prev, double progress, ZParameter &dest)
{
  Q_UNUSED(progress)
  assert(this->isSameType(prev) && this->isSameType(dest));
}

QJsonValue ZEventListenerParameter::jsonValue() const
{
  return QJsonValue(QString(""));
}

void ZEventListenerParameter::readValue(const QJsonValue &)
{
}

} // namespace nim
