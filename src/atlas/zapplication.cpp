#include "zapplication.h"
#include "zexception.h"
#include "zlog.h"
#include "zmessageboxhelpers.h"
#include <QFileOpenEvent>

namespace nim {

bool ZApplication::notify(QObject* object, QEvent* event)
{
  try {
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
