#include "ztracesettings.h"

namespace nim {

ZTraceSettings::ZTraceSettings(QObject* parent)
  : QObject(parent)
{}

ZTraceSettings::SwcTargetSelection ZTraceSettings::mappedSwcTargetSelection(std::optional<size_t> sourceImageId,
                                                                            size_t sourceChannel) const
{
  if (!sourceImageId.has_value()) {
    return {};
  }

  const SourceKey key{*sourceImageId, sourceChannel};
  const auto it = m_swcTargetBySource.find(key);
  if (it == m_swcTargetBySource.end()) {
    return {};
  }
  return it->second;
}

void ZTraceSettings::setSourceImageId(std::optional<size_t> id)
{
  setSourceSelection(id, m_sourceChannel);
}

void ZTraceSettings::setSourceChannel(size_t sc)
{
  setSourceSelection(m_sourceImageId, sc);
}

void ZTraceSettings::setSourceSelection(std::optional<size_t> sourceImageId, size_t sourceChannel)
{
  bool anyChanged = false;

  if (sourceImageId != m_sourceImageId) {
    m_sourceImageId = sourceImageId;
    anyChanged = true;
  }

  if (sourceChannel != m_sourceChannel) {
    m_sourceChannel = sourceChannel;
    anyChanged = true;
  }

  SwcTargetSelection target = mappedSwcTargetSelection(sourceImageId, sourceChannel);
  if (target.mode != m_swcTargetMode) {
    m_swcTargetMode = target.mode;
    anyChanged = true;
  }

  if (target.mode == SwcTargetMode::NewSwc) {
    target.swcId = std::nullopt;
  }

  if (target.swcId != m_targetSwcId) {
    m_targetSwcId = target.swcId;
    anyChanged = true;
  }

  if (anyChanged) {
    Q_EMIT changed();
  }
}

void ZTraceSettings::setSwcTargetMode(SwcTargetMode mode)
{
  setTargetSelection(mode, m_targetSwcId);
}

void ZTraceSettings::setTargetSwcId(std::optional<size_t> id)
{
  setTargetSelection(m_swcTargetMode, id);
}

void ZTraceSettings::setTargetSelection(SwcTargetMode swcTargetMode, std::optional<size_t> targetSwcId)
{
  if (swcTargetMode == SwcTargetMode::NewSwc) {
    targetSwcId = std::nullopt;
  }

  bool anyChanged = false;

  if (swcTargetMode != m_swcTargetMode) {
    m_swcTargetMode = swcTargetMode;
    anyChanged = true;
  }

  if (targetSwcId != m_targetSwcId) {
    m_targetSwcId = targetSwcId;
    anyChanged = true;
  }

  if (m_sourceImageId.has_value()) {
    const SourceKey key{*m_sourceImageId, m_sourceChannel};
    const SwcTargetSelection selection{m_swcTargetMode, m_targetSwcId};
    const auto it = m_swcTargetBySource.find(key);
    if (it == m_swcTargetBySource.end() || !(it->second == selection)) {
      m_swcTargetBySource[key] = selection;
    }
  }

  if (anyChanged) {
    Q_EMIT changed();
  }
}

void ZTraceSettings::setSelection(std::optional<size_t> sourceImageId,
                                  size_t sourceChannel,
                                  SwcTargetMode swcTargetMode,
                                  std::optional<size_t> targetSwcId)
{
  if (swcTargetMode == SwcTargetMode::NewSwc) {
    targetSwcId = std::nullopt;
  }

  bool anyChanged = false;

  if (sourceImageId != m_sourceImageId) {
    m_sourceImageId = sourceImageId;
    anyChanged = true;
  }

  if (sourceChannel != m_sourceChannel) {
    m_sourceChannel = sourceChannel;
    anyChanged = true;
  }

  if (swcTargetMode != m_swcTargetMode) {
    m_swcTargetMode = swcTargetMode;
    anyChanged = true;
  }

  if (targetSwcId != m_targetSwcId) {
    m_targetSwcId = targetSwcId;
    anyChanged = true;
  }

  if (sourceImageId.has_value()) {
    const SourceKey key{*sourceImageId, sourceChannel};
    const SwcTargetSelection selection{m_swcTargetMode, m_targetSwcId};
    const auto it = m_swcTargetBySource.find(key);
    if (it == m_swcTargetBySource.end() || !(it->second == selection)) {
      m_swcTargetBySource[key] = selection;
    }
  }

  if (anyChanged) {
    Q_EMIT changed();
  }
}

void ZTraceSettings::promoteNewSwcTargetToExistingIfStillNew(size_t sourceImageId,
                                                             size_t sourceChannel,
                                                             size_t newSwcId)
{
  const SourceKey key{sourceImageId, sourceChannel};

  const auto it = m_swcTargetBySource.find(key);
  if (it != m_swcTargetBySource.end() && it->second.mode != SwcTargetMode::NewSwc) {
    return;
  }

  m_swcTargetBySource[key] = SwcTargetSelection{SwcTargetMode::ExistingSwc, std::optional<size_t>(newSwcId)};

  if (!m_sourceImageId.has_value() || *m_sourceImageId != sourceImageId || m_sourceChannel != sourceChannel) {
    return;
  }

  if (m_swcTargetMode != SwcTargetMode::NewSwc) {
    return;
  }

  bool anyChanged = false;
  if (m_swcTargetMode != SwcTargetMode::ExistingSwc) {
    m_swcTargetMode = SwcTargetMode::ExistingSwc;
    anyChanged = true;
  }
  const std::optional<size_t> swcIdOpt = std::optional<size_t>(newSwcId);
  if (m_targetSwcId != swcIdOpt) {
    m_targetSwcId = swcIdOpt;
    anyChanged = true;
  }

  if (anyChanged) {
    Q_EMIT changed();
  }
}

void ZTraceSettings::setTraceToolEnabled(bool enabled)
{
  if (enabled == m_traceToolEnabled) {
    return;
  }
  m_traceToolEnabled = enabled;
  Q_EMIT changed();
}

void ZTraceSettings::setTraceInProgress(bool inProgress)
{
  if (inProgress == m_traceInProgress) {
    return;
  }
  m_traceInProgress = inProgress;
  Q_EMIT changed();
}

void ZTraceSettings::initializeAlgoConfigIfUnset(const AlgoConfig& cfg)
{
  if (m_algoConfigInitialized) {
    return;
  }
  m_algoConfigInitialized = true;
  m_algoConfig = cfg;
  Q_EMIT changed();
}

void ZTraceSettings::setAlgoConfig(const AlgoConfig& cfg)
{
  if (m_algoConfigInitialized && cfg == m_algoConfig) {
    return;
  }
  m_algoConfigInitialized = true;
  m_algoConfig = cfg;
  Q_EMIT changed();
}

} // namespace nim
