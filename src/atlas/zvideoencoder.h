#pragma once

#include "zprocess.h"
#include <QDir>
#include <cstddef>
#include <optional>

namespace nim {

class ZVideoEncoder : public ZProcess
{
  Q_OBJECT

public:
  explicit ZVideoEncoder(QObject* parent = nullptr);

  // A missing frameCount consumes the remaining contiguous frame sequence.
  static std::tuple<QString, QStringList> encodeDryRun(const QDir& dir,
                                                       const QString& namePrefix,
                                                       int fieldWidth,
                                                       int framesPerSecond,
                                                       int startFrame,
                                                       std::optional<size_t> frameCount,
                                                       const QString& outputFilename);

  void encode(const QDir& dir,
              const QString& namePrefix,
              int fieldWidth,
              int framesPerSecond,
              int startFrame,
              std::optional<size_t> frameCount,
              const QString& outputFilename);
};

} // namespace nim
