#pragma once

#include <QObject>

namespace nim {

class ZRunDumpAnimation3DSchema : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;

  // Returns 0 on success, non-zero on failure
  int run();
};

} // namespace nim

