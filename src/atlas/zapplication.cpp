#include "zapplication.h"
#include "zexception.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"
#include <QCoreApplication>
#include <QFileOpenEvent>
#include <QThread>

namespace nim {

bool ZApplication::notify(QObject* object, QEvent* event)
{
  try {
    // Fast path: the overwhelming majority of Qt event delivery happens on the
    // GUI thread. Keep this branch as cheap as possible.
    if (Q_LIKELY(QThread::currentThread() == this->thread())) {
      return QApplication::notify(object, event);
    }

    // NOTE: Atlas posts mouse/keyboard/wheel events across threads (UI thread → rendering thread)
    // so the rendering engine can process interactions without blocking the UI.
    //
    // Qt Widgets' QApplication::notify() contains GUI-input bookkeeping that assumes delivery on
    // the GUI thread (e.g. double-click / input timers). When these input events are delivered on
    // a non-GUI thread, that bookkeeping can attempt to stop a GUI-thread timer and trigger:
    //   "QBasicTimer::stop: Failed. Possibly trying to stop from a different thread"
    //
    // For non-GUI threads we bypass QApplication's widget-specific input handling and use the
    // QCoreApplication::notify() implementation, which still runs event filters and ultimately
    // calls QObject::event() on the receiver without touching GUI-thread timers.
    if (!event) {
      return QCoreApplication::notify(object, event);
    }

    switch (event->type()) {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonRelease:
      case QEvent::MouseButtonDblClick:
      case QEvent::MouseMove:
      case QEvent::Wheel:
      case QEvent::KeyPress:
      case QEvent::KeyRelease:
      case QEvent::ContextMenu:
        return QCoreApplication::notify(object, event);
      default:
        break;
    }

    // Non-input events: preserve QApplication behavior (style, widget plumbing, etc.)
    // even when delivered on a non-GUI thread.
    return QApplication::notify(object, event);
  }
  catch (const ZException& e) {
    QString err = QString("Uncaught %1 <%2> while sending event %3 to object %4 (%5)")
                    .arg(typeid(e).name(),
                         e.what(),
                         typeid(*event).name(),
                         object->objectName(),
                         object->metaObject()->className());
    showCriticalWithDetails(activeWindow(), tr("Unhandled exception"), err);
    LOG(FATAL) << err;
  }
  catch (const std::exception& e) {
    QString err = QString("Uncaught %1 <%2> while sending event %3 to object %4 (%5)")
                    .arg(typeid(e).name(),
                         e.what(),
                         typeid(*event).name(),
                         object->objectName(),
                         object->metaObject()->className());
    showCriticalWithDetails(activeWindow(), tr("Unhandled exception"), err);
    LOG(FATAL) << err;
  }

  return false;
}

bool ZApplication::event(QEvent* event)
{
  if (event->type() == QEvent::FileOpen) {
    auto openEvent = static_cast<QFileOpenEvent*>(event);
    LOG(INFO) << "Open file: " << openEvent->file();
    QList<QUrl> list;
    list << openEvent->url();
    Q_EMIT fileOpenRequest(list);
  }
  return QApplication::event(event);
}

} // namespace nim
