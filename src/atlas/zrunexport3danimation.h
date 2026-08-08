#pragma once

#include <QObject>
#include <QStringList>
#include <cstddef>
#include <utility>
#include <vector>

class QString;

namespace nim {

class Z3DRenderingEngine;

class ZRunExport3DAnimation : public QObject
{
  Q_OBJECT

public:
  using FrameRange = std::pair<int, int>;

  using QObject::QObject;

  // Split the half-open interval [startFrame, endFrame) into balanced,
  // nonempty ranges. The returned range count is capped by maxWorkerCount.
  [[nodiscard]] static std::vector<FrameRange> splitFrameRange(int startFrame, int endFrame, size_t maxWorkerCount);

  int run(const QStringList& childQpaPlatformArguments);

protected:
  void logError(const QString& err);

private:
  bool m_hasError = false;
  Z3DRenderingEngine* m_engine = nullptr;
};

} // namespace nim
