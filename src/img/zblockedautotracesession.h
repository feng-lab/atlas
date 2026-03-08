#pragma once

#include "zneutubelocalneuroseg.h"
#include "zneutubetraceconfig.h"
#include "zneutubetracedefs.h"

#include "zjson.h"
#include "zswc.h"

#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nim {

inline constexpr int kBlockedAutoTraceManifestFormatVersion = 2;
inline constexpr int kBlockedAutoTraceCommitFormatVersion = 1;

// Crash-safe, append-only checkpoint IO for blocked auto tracing.
//
// Design goals:
// - Never overwrite previously committed results.
// - Resume from the last fully committed checkpoint, ignoring half-written ones.
// - Keep the persisted format explicit and versioned (JSON + SWC delta fragments).
//
// Note: This is intentionally UI-agnostic; callers provide the exact dataset identity and effective TraceConfig.

struct ZBlockedAutoTraceBlockSize
{
  int64_t coreX = 0;
  int64_t coreY = 0;
  int64_t coreZ = 0;
  int64_t halo = 0;
};

struct ZBlockedAutoTraceDatasetShape
{
  int64_t width = 0;
  int64_t height = 0;
  int64_t depth = 0;
};

struct ZBlockedAutoTraceBlockId
{
  int64_t bx = 0;
  int64_t by = 0;
  int64_t bz = 0;
};

struct ZBlockedAutoTraceSchedulerState
{
  // Linear scan cursor for picking the next never-visited block when frontier is empty.
  uint64_t nextLinearBlockIndex = 0;
};

struct ZBlockedAutoTracePendingTask
{
  uint64_t taskId = 0;
  int64_t attachSwcNodeId = -1;
  TraceDirection direction = TraceDirection::Unknown; // Forward/Backward
  LocalNeuroseg endLocseg{};

  // Optional diagnostics; not used for control flow.
  std::string reason;
  ZBlockedAutoTraceBlockId suggestedBlock{};
};

struct ZBlockedAutoTraceManifest
{
  // Increment when the on-disk schema changes incompatibly.
  int formatVersion = kBlockedAutoTraceManifestFormatVersion;

  // Exact dataset identity for resume compatibility.
  // Atlas stores the JSON serialization of `ZImgSource` here and refuses to resume across mismatched sources.
  std::string datasetId;

  size_t channel = 0;
  size_t time = 0;

  // Downsample ratio applied when reading ROIs for tracing. This also defines the coordinate system for the persisted
  // SWC deltas and pending tasks.
  //
  // ratio=[1,1,1] means full resolution.
  std::array<size_t, 3> signalDownsampleRatio = {1, 1, 1};

  // Explicit tracing anisotropy in tracing-voxel coordinates:
  //   zScale = voxelSizeZ / voxelSizeXY
  // This is part of session identity and must remain stable across resume.
  double zScale = 1.0;

  ZBlockedAutoTraceDatasetShape datasetShape{};
  ZBlockedAutoTraceBlockSize block{};

  // Preprocess policy (block-invariant, resumable):
  //
  // - thresholdMode="fixed": subtract `subtractConstant` from the signal ROI before mask/seed detection and tracing.
  //   This matches the previous blocked-tracer behavior. `subtractConstant=0` means no-op.
  //
  // - thresholdMode="auto": subtract background per block ROI using the legacy neuTube `SubtractBackground` algorithm
  //   (ported as `subtractBackgroundLegacyLike`). The fixed constant is ignored in this mode.
  std::string thresholdMode = "fixed"; // "fixed" | "auto"
  double subtractConstant = 0.0; // used only when thresholdMode=="fixed"

  // Effective trace config (after any per-level overrides are applied).
  TraceConfig traceConfig{};
};

struct ZBlockedAutoTraceCommitInfo
{
  int formatVersion = kBlockedAutoTraceCommitFormatVersion;
  uint64_t commitId = 0;

  ZBlockedAutoTraceBlockId blockId{};
  bool didSeedScan = false;

  size_t newSwcNodes = 0;
  size_t pendingTasks = 0;
};

struct ZBlockedAutoTraceSwcDeltaNode
{
  int64_t id = -1;
  int64_t type = 0;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double radius = 0.0;
  int64_t parentId = -1;
};

struct ZBlockedAutoTraceCommitWrite
{
  ZBlockedAutoTraceCommitInfo info{};
  std::vector<ZBlockedAutoTraceSwcDeltaNode> swcDeltaNodes;
  std::vector<ZBlockedAutoTracePendingTask> frontier;
  ZBlockedAutoTraceSchedulerState scheduler;
};

struct ZBlockedAutoTraceLoadedState
{
  ZBlockedAutoTraceManifest manifest{};
  uint64_t commitId = 0;

  ZSwc swc;
  std::unordered_map<int64_t, ZSwc::SwcTreeNode> nodeById;

  std::vector<ZBlockedAutoTracePendingTask> frontier;
  ZBlockedAutoTraceSchedulerState scheduler{};

  // Core blocks for which seed detection has been applied at least once.
  std::vector<ZBlockedAutoTraceBlockId> seedScannedBlocks;
};

class ZBlockedAutoTraceSession final
{
public:
  explicit ZBlockedAutoTraceSession(QString sessionDir);

  [[nodiscard]] const QString& sessionDir() const
  {
    return m_sessionDir;
  }

  void ensureCreatedOrThrow(const ZBlockedAutoTraceManifest& manifest);

  // Finds the highest fully committed commit id, requiring a contiguous chain from 1..N.
  [[nodiscard]] uint64_t latestCommittedIdOrZero() const;

  // Loads manifest + state up to latest committed (or empty state for new session).
  [[nodiscard]] ZBlockedAutoTraceLoadedState loadLatestOrEmptyOrThrow() const;

  // Loads only the manifest (no commit replay). Throws if the manifest is missing or invalid.
  [[nodiscard]] ZBlockedAutoTraceManifest loadManifestOrThrow() const;

  // Appends a new commit (must be commitId == latestCommitted+1).
  void writeCommitOrThrow(const ZBlockedAutoTraceCommitWrite& commit);

  // Convenience artifact: an append-only SWC file containing all committed delta nodes so far.
  //
  // - Stored at `<sessionDir>/result_tracing.swc`.
  // - Coordinates are in the tracing voxel space (after `signal_downsample_ratio`).
  // - Node IDs remain stable and must not be resampled/renumbered.
  //
  // This file is *not* required for correctness (canonical resume data is the per-commit `swc_delta.swc`),
  // but it makes progress inspection and crash recovery faster.
  void appendToRollingSwcOrThrow(uint64_t commitId, const std::vector<ZBlockedAutoTraceSwcDeltaNode>& nodes) const;

private:
  [[nodiscard]] QString manifestPath() const;
  [[nodiscard]] QString blocksDirPath() const;
  [[nodiscard]] QString rollingSwcPath() const;
  [[nodiscard]] QString rollingSwcStatePath() const;

  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceBlockSize& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceDatasetShape& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceBlockId& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceSchedulerState& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceManifest& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTracePendingTask& v);
  [[nodiscard]] static json::object toJson(const ZBlockedAutoTraceCommitInfo& v);

  static ZBlockedAutoTraceBlockSize blockSizeFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTraceDatasetShape datasetShapeFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTraceBlockId blockIdFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTraceSchedulerState schedulerFromJsonOrThrow(const json::object& o);
  static TraceConfig traceConfigFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTraceManifest manifestFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTracePendingTask pendingTaskFromJsonOrThrow(const json::object& o);
  static ZBlockedAutoTraceCommitInfo commitInfoFromJsonOrThrow(const json::object& o);

  // (see public API)
  void writeManifestAtomicOrThrow(const ZBlockedAutoTraceManifest& manifest) const;

  struct CommitDir
  {
    uint64_t id = 0;
    QString path;
  };

  [[nodiscard]] std::vector<CommitDir> listCommittedDirsSortedOrThrow() const;

  [[nodiscard]] static QString commitDirName(uint64_t commitId);
  [[nodiscard]] static QString commitJsonName();
  [[nodiscard]] static QString frontierJsonName();
  [[nodiscard]] static QString schedulerJsonName();
  [[nodiscard]] static QString swcDeltaName();

  [[nodiscard]] static std::vector<ZBlockedAutoTraceSwcDeltaNode> readSwcDeltaOrThrow(const QString& filePath);
  static void writeSwcDeltaOrThrow(const QString& filePath, const std::vector<ZBlockedAutoTraceSwcDeltaNode>& nodes);
  static void applySwcDeltaOrThrow(const std::vector<ZBlockedAutoTraceSwcDeltaNode>& delta,
                                   ZSwc& swc,
                                   std::unordered_map<int64_t, ZSwc::SwcTreeNode>& nodeById);

private:
  QString m_sessionDir;
};

} // namespace nim
