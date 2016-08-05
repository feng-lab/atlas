//
// Created by Linqing Feng on 8/5/16.
//

#include "zapplication.h"
#include "zexception.h"
#include <QMessageBox>
#include "zlog.h"

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

} // namespace nim
