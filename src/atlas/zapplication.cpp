#include "zapplication.h"

#include "zexception.h"
#include "zlog.h"
#include <QMessageBox>
#include <QFileOpenEvent>

namespace nim {


bool ZApplication::notify(QObject* object, QEvent* event)
{
  try {
    return QApplication::notify(object, event);
  }
  catch (const ZException& e) {
    QString err = QString("Uncaught %1 <%2> while sending event %3 to object %4 (%5)")
      .arg(typeid(e).name()).arg(e.what())
      .arg(typeid(*event).name())
      .arg(object->objectName()).arg(object->metaObject()->className());
    QMessageBox::critical(activeWindow(), applicationName(), err);
    LOG(FATAL) << err;
  }
  catch (const std::exception& e) {
    QString err = QString("Uncaught %1 <%2> while sending event %3 to object %4 (%5)")
      .arg(typeid(e).name()).arg(e.what())
      .arg(typeid(*event).name())
      .arg(object->objectName()).arg(object->metaObject()->className());
    QMessageBox::critical(activeWindow(), applicationName(), err);
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
    emit fileOpenRequest(list);
  }
  return QApplication::event(event);
}

QString ZApplication::resourcesDirPath()
{
#ifdef Q_OS_MACOS
  return applicationDirPath() + u"/../Resources";
#else
  return applicationDirPath() + u"/Resources";
#endif
}

QString ZApplication::applicationInstallDirPath()
{
#ifdef Q_OS_MACOS
  return applicationDirPath() + u"/../../..";
#elif defined(Q_OS_WIN64)
  return applicationDirPath() + u"/..";
#else
  return applicationDirPath() + u"/..";
#endif
}

} // namespace nim
