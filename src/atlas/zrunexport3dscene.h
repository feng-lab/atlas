#pragma once

#include <QObject>

class QString;

namespace nim {

class ZRunExport3DScene : public QObject
{
  Q_OBJECT

public:
  using QObject::QObject;

  int run();

protected:
  void logError(const QString& err);

private:
  bool m_hasError = false;
};

} // namespace nim
