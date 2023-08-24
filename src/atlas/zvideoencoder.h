#pragma once

#include "zprocess.h"
#include <QDir>

namespace nim {

class ZVideoEncoder : public ZProcess
{
  Q_OBJECT

public:
  explicit ZVideoEncoder(QObject* parent = nullptr);

  static std::tuple<QString, QStringList> encodeDryRun(const QDir& dir,
                                                       const QString& namePrefix,
                                                       int fieldWidth,
                                                       int framesPerSecond,
                                                       const QString& outputFilename);

  void encode(const QDir& dir,
              const QString& namePrefix,
              int fieldWidth,
              int framesPerSecond,
              const QString& outputFilename);
};

} // namespace nim
