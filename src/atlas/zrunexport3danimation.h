#pragma once

#include <QObject>
#include <QStringList>
#include <cstddef>
#include <cstdint>
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

  // Split the half-open interval [startFrame, endFrame) into adjacent,
  // nonempty ranges. The result has at most one range per frame. Empty weights
  // produce balanced ranges; otherwise every requested worker has one positive
  // relative weight, and only weights for workers with a range participate.
  [[nodiscard]] static std::vector<FrameRange>
  splitFrameRange(int startFrame, int endFrame, size_t maxWorkerCount, const std::vector<uint32_t>& workerWeights = {});

  int run(const QStringList& childQpaPlatformArguments);

protected:
  void logError(const QString& err);

private:
  bool m_hasError = false;
  Z3DRenderingEngine* m_engine = nullptr;
};

} // namespace nim
