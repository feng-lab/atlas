#pragma once

#include "zimg.h"
#include "zimgprocess.h"
#include "zswc.h"
#include "zneutubetraceconfig.h"

#include <folly/CancellationToken.h>

#include <array>
#include <cmath>
#include <optional>
#include <functional>

namespace nim {

// ZImgProcess wrapper for the neuTube legacy auto-trace algorithm.
//
// This allows Atlas UI and Python entry points to launch auto tracing via the shared
// ZImgProcessDialog / background task framework (progress, cancellation, Tasks panel).
class ZNeutubeAutoTraceProcess final : public ZImgProcess
{
public:
  // Returns the selected single-channel/single-time signal image to trace.
  // The provider is invoked on the background thread during doWork().
  using SignalProvider = std::function<ZImg(folly::CancellationToken)>;

  ZNeutubeAutoTraceProcess() = default;

  void setSignalProvider(SignalProvider provider)
  {
    m_signalProvider = std::move(provider);
  }

  void setSelectedChannelTime(size_t sc, size_t t)
  {
    m_selectedChannel = sc;
    m_selectedTime = t;
  }

  void setZToXYRatio(double zToXYRatio)
  {
    CHECK(std::isfinite(zToXYRatio));
    CHECK(zToXYRatio > 0.0);
    m_zToXYRatio = zToXYRatio;
  }

  void setTraceConfigPath(QString path)
  {
    m_traceConfigPath = std::move(path);
  }

  // 0 means "default" (no per-level override), matching NeuTu semantics.
  void setTraceLevel(int level)
  {
    m_traceLevel = level;
  }

  void setAlgoConfigOverrides(const TraceConfig& cfg)
  {
    m_algoOverrides = cfg;
    m_haveAlgoOverrides = true;
  }

  void clearAlgoConfigOverrides()
  {
    m_haveAlgoOverrides = false;
  }

  void setDoResampleAfterTracing(bool enabled)
  {
    m_doResampleAfterTracing = enabled;
  }

  // If the signal image is downsampled before tracing, the traced SWC coordinates
  // are in the downsampled voxel space. Set this ratio so the result is rescaled
  // back to the original image coordinates before writing.
  void setSignalDownsampleRatio(std::array<size_t, 3> ratio)
  {
    CHECK(ratio[0] > 0);
    CHECK(ratio[1] > 0);
    CHECK(ratio[2] > 0);
    m_signalDownsampleRatio = ratio;
  }

  void setDocHasAnySwc(bool v)
  {
    m_docHasAnySwc = v;
  }

  void setOutputSwcPath(QString path)
  {
    m_outputSwcPath = std::move(path);
  }

  [[nodiscard]] const QString& outputSwcPath() const
  {
    return m_outputSwcPath;
  }

  // True if a non-empty SWC was produced and written to outputSwcPath().
  [[nodiscard]] bool hasResult() const
  {
    return m_hasResult;
  }

protected:
  void doWork() override;

  void read(const json::object& jo) override;

  void write(json::object& jo) const override;

private:
  [[nodiscard]] ZImg loadSelectedSignalOrThrow() const;

  [[nodiscard]] TraceConfig buildEffectiveTraceConfigOrThrow() const;

  void writeSwcAtomicOrThrow(ZSwc& tree) const;

private:
  SignalProvider m_signalProvider;

  size_t m_selectedChannel = 0;
  size_t m_selectedTime = 0;
  std::optional<double> m_zToXYRatio;

  QString m_traceConfigPath;
  int m_traceLevel = 0;

  bool m_haveAlgoOverrides = false;
  TraceConfig m_algoOverrides;

  bool m_doResampleAfterTracing = true;
  bool m_docHasAnySwc = false;

  std::array<size_t, 3> m_signalDownsampleRatio = {1, 1, 1};

  QString m_outputSwcPath;

  bool m_hasResult = false;
};

} // namespace nim
