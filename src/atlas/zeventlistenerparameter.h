#pragma once

#include "zparameter.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <utility>

namespace nim {

class ZEventListenerParameter : public ZParameter
{
Q_OBJECT
public:
  explicit ZEventListenerParameter(const QString& name, bool sharing = false, QObject* parent = nullptr);

  inline void setSharing(bool s)
  { if (m_sharing != s) { m_sharing = s; Q_EMIT valueChanged(); }}

  [[nodiscard]] inline bool isSharing() const
  { return m_sharing; }

  // buttons and modifiers should be exact match with the input to trigger the event signal
  // An OR-combinations of buttons means all buttons should be pressed at the time
  // An OR-combinations of modifiers means all modifiers should be pressed at the time
  void
  listenTo(const QString& actionName, const Qt::MouseButtons& buttons, const Qt::KeyboardModifiers& modifiers,
           QEvent::Type type);

  //
  void listenTo(const QString& actionName, Qt::Key key, const Qt::KeyboardModifiers& modifiers,
                QEvent::Type type = QEvent::KeyPress);

  void listenToContextMenuEvent()
  { if (!m_listeningToContextMenuEvent) { m_listeningToContextMenuEvent = true; Q_EMIT valueChanged(); } }

  void clearAll();

  void sendEvent(QEvent* e, int w, int h);

  // ZParameter interface
public:
  void setSameAs(const ZParameter& rhs) override;

  void setValueSameAs(const ZParameter& /*rhs*/) override;

  void interpolate(const ZParameter& prev, double progress, ZParameter& dest) override;

  [[nodiscard]] json::value jsonValue() const override;

  void readValue(const json::value& jsonValue) override;

Q_SIGNALS:

  void eventTriggered(QEvent* e, int w, int h);

  void mouseEventTriggered(QMouseEvent* e, int w, int h);

  void keyEventTriggered(QKeyEvent* e, int w, int h);

  void wheelEventTriggered(QWheelEvent* e, int w, int h);

  void contextMenuEventTriggered(QContextMenuEvent* e, int w, int h);

protected:
  struct MouseEvent
  {
    MouseEvent(QString  actionName_, Qt::MouseButtons buttons_,
               Qt::KeyboardModifiers modifiers_, QEvent::Type type_)
      : actionName(std::move(actionName_)), buttons(buttons_), modifiers(modifiers_), type(type_)
    {}

    QString actionName;
    Qt::MouseButtons buttons;
    Qt::KeyboardModifiers modifiers;
    QEvent::Type type;
  };

  struct KeyEvent
  {
    KeyEvent(QString  actionName_, Qt::Key key_, Qt::KeyboardModifiers modifiers_,
             QEvent::Type type_ = QEvent::KeyPress)
      : actionName(std::move(actionName_)), key(key_), modifiers(modifiers_), type(type_)
    {}

    QString actionName;
    Qt::Key key;
    Qt::KeyboardModifiers modifiers;
    QEvent::Type type;
  };

  QWidget* actualCreateWidget(QWidget* parent) override;

private:
  bool m_sharing;
  std::vector<MouseEvent> m_mouseEvents;
  std::vector<KeyEvent> m_keyEvents;
  bool m_listeningToContextMenuEvent = false;
};

} // namespace nim

