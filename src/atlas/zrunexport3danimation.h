#pragma once

#include <QObject>

class QString;

namespace nim {

class ZRunExport3DAnimation : public QObject
{
  Q_OBJECT

public:
  using QObject::QObject;

  static int run();

protected:
  static void logError(const QString& err);
};

} // namespace nim
