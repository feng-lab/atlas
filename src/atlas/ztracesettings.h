#pragma once

#include <QObject>
#include <cstddef>
#include <map>
#include <optional>

namespace nim {

class ZTraceSettings : public QObject
{
  Q_OBJECT

public:
  struct AlgoConfig
  {
    // Defaults match `nim::TraceConfig{}` (ported from neuTube's `ZNeuronTracerConfig::init()`).
    double minAutoScore = 0.3;
    double minManualScore = 0.3;
    double minSeedScore = 0.35;
    double min2dScore = 0.5;

    bool refit = false;
    bool spTest = true;
    bool crossoverTest = false;
    bool tuneEnd = true;
    bool edgePath = false;
    bool enhanceMask = false;

    int seedMethod = 1;
    int recover = 2;
    int chainScreenCount = 0;
    double maxEucDist = 20.0;

    bool operator==(const AlgoConfig&) const = default;
  };

  enum class SwcTargetMode
  {
    NewSwc,
    ExistingSwc,
  };
  Q_ENUM(SwcTargetMode)

  explicit ZTraceSettings(QObject* parent = nullptr);

  [[nodiscard]] std::optional<size_t> sourceImageId() const
  {
    return m_sourceImageId;
  }

  void setSourceImageId(std::optional<size_t> id);

  [[nodiscard]] size_t sourceChannel() const
  {
    return m_sourceChannel;
  }

  void setSourceChannel(size_t sc);

  // Updates the source selection and loads the persisted SWC target mapping for
  // that (image, channel) pair (session-only, in-memory).
  void setSourceSelection(std::optional<size_t> sourceImageId, size_t sourceChannel);

  [[nodiscard]] SwcTargetMode swcTargetMode() const
  {
    return m_swcTargetMode;
  }

  void setSwcTargetMode(SwcTargetMode mode);

  [[nodiscard]] std::optional<size_t> targetSwcId() const
  {
    return m_targetSwcId;
  }

  void setTargetSwcId(std::optional<size_t> id);

  // Updates the SWC target selection for the current (image, channel) pair and
  // persists it in the session-only mapping.
  void setTargetSelection(SwcTargetMode swcTargetMode, std::optional<size_t> targetSwcId);

  // Convenience for updating multiple fields at once (emits `changed()` at most once).
  void setSelection(std::optional<size_t> sourceImageId,
                    size_t sourceChannel,
                    SwcTargetMode swcTargetMode,
                    std::optional<size_t> targetSwcId);

  // UX helper: when a trace is started with "New SWC", promote that choice to
  // "Existing SWC" after the first successful trace, but only if the user hasn't
  // already mapped that (image, channel) pair to some other target.
  void promoteNewSwcTargetToExistingIfStillNew(size_t sourceImageId, size_t sourceChannel, size_t newSwcId);

  [[nodiscard]] bool traceToolEnabled() const
  {
    return m_traceToolEnabled;
  }

  void setTraceToolEnabled(bool enabled);

  [[nodiscard]] bool traceInProgress() const
  {
    return m_traceInProgress;
  }

  void setTraceInProgress(bool inProgress);

  [[nodiscard]] bool algoConfigInitialized() const
  {
    return m_algoConfigInitialized;
  }

  [[nodiscard]] AlgoConfig algoConfig() const
  {
    return m_algoConfig;
  }

  void initializeAlgoConfigIfUnset(const AlgoConfig& cfg);
  void setAlgoConfig(const AlgoConfig& cfg);

Q_SIGNALS:
  void changed();

private:
  struct SourceKey
  {
    size_t imageId = 0;
    size_t channel = 0;

    bool operator<(const SourceKey& other) const
    {
      if (imageId != other.imageId) {
        return imageId < other.imageId;
      }
      return channel < other.channel;
    }
  };

  struct SwcTargetSelection
  {
    SwcTargetMode mode = SwcTargetMode::NewSwc;
    std::optional<size_t> swcId;

    bool operator==(const SwcTargetSelection&) const = default;
  };

  [[nodiscard]] SwcTargetSelection mappedSwcTargetSelection(std::optional<size_t> sourceImageId,
                                                            size_t sourceChannel) const;

  std::optional<size_t> m_sourceImageId;
  size_t m_sourceChannel = 0;
  SwcTargetMode m_swcTargetMode = SwcTargetMode::NewSwc;
  std::optional<size_t> m_targetSwcId;
  std::map<SourceKey, SwcTargetSelection> m_swcTargetBySource;
  bool m_traceToolEnabled = false;
  bool m_traceInProgress = false;
  bool m_algoConfigInitialized = false;
  AlgoConfig m_algoConfig;
};

} // namespace nim
