#include "zblockedautotracesession.h"

#include "zexception.h"
#include "zlog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>

#include <folly/ScopeGuard.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] QString ensureDirPath(const QString& p)
{
  // Make it easy for callers to pass either "foo" or "foo/".
  QString out = QDir::cleanPath(p);
  return out;
}

[[nodiscard]] std::string_view traceDirectionToString(TraceDirection d)
{
  switch (d) {
    case TraceDirection::Forward:
      return "Forward";
    case TraceDirection::Backward:
      return "Backward";
    case TraceDirection::BothDir:
      return "BothDir";
    case TraceDirection::Unknown:
      return "Unknown";
  }
  return "Unknown";
}

[[nodiscard]] TraceDirection traceDirectionFromStringOrThrow(std::string_view s)
{
  if (s == "Forward") {
    return TraceDirection::Forward;
  }
  if (s == "Backward") {
    return TraceDirection::Backward;
  }
  if (s == "BothDir") {
    return TraceDirection::BothDir;
  }
  if (s == "Unknown") {
    return TraceDirection::Unknown;
  }
  throw ZException(fmt::format("Invalid TraceDirection string: '{}'", std::string(s)));
}

template<typename T>
[[nodiscard]] T requireNumberOrThrow(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    throw ZException(fmt::format("Missing key '{}'", key));
  }
  if (!it->value().is_number()) {
    throw ZException(fmt::format("Invalid '{}' type: expected number, got {}", key, jsonTypeName(it->value())));
  }
  return json::value_to<T>(it->value());
}

[[nodiscard]] bool requireBoolOrThrow(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    throw ZException(fmt::format("Missing key '{}'", key));
  }
  if (!it->value().is_bool()) {
    throw ZException(fmt::format("Invalid '{}' type: expected bool, got {}", key, jsonTypeName(it->value())));
  }
  return it->value().as_bool();
}

[[nodiscard]] std::string requireStringOrThrow(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    throw ZException(fmt::format("Missing key '{}'", key));
  }
  if (!it->value().is_string()) {
    throw ZException(fmt::format("Invalid '{}' type: expected string, got {}", key, jsonTypeName(it->value())));
  }
  const auto& s = it->value().as_string();
  return std::string(s.data(), s.size());
}

[[nodiscard]] json::object requireObjectOrThrow(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    throw ZException(fmt::format("Missing key '{}'", key));
  }
  if (!it->value().is_object()) {
    throw ZException(fmt::format("Invalid '{}' type: expected object, got {}", key, jsonTypeName(it->value())));
  }
  return it->value().as_object();
}

[[nodiscard]] json::array requireArrayOrThrow(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    throw ZException(fmt::format("Missing key '{}'", key));
  }
  if (!it->value().is_array()) {
    throw ZException(fmt::format("Invalid '{}' type: expected array, got {}", key, jsonTypeName(it->value())));
  }
  return it->value().as_array();
}

[[nodiscard]] std::optional<json::object> findObject(const json::object& o, const char* key)
{
  auto it = o.find(key);
  if (it == o.end()) {
    return std::nullopt;
  }
  if (!it->value().is_object()) {
    return std::nullopt;
  }
  return it->value().as_object();
}

void atomicWriteJsonObjectOrThrow(const QString& finalPath, const json::object& jo)
{
  QSaveFile file(finalPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to open temp JSON file: %1").arg(finalPath));
  }

  const std::string payload = jsonToFormattedString(jo);
  const qint64 written = file.write(payload.data(), static_cast<qint64>(payload.size()));
  if (written != static_cast<qint64>(payload.size())) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to write JSON file: %1").arg(finalPath));
  }

  if (!file.commit()) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to commit JSON file: %1").arg(finalPath));
  }
}

[[nodiscard]] LocalNeuroseg localNeurosegFromJsonOrThrow(const json::object& o)
{
  LocalNeuroseg out;

  const json::object seg = requireObjectOrThrow(o, "seg");
  out.seg.r1 = requireNumberOrThrow<double>(seg, "r1");
  out.seg.c = requireNumberOrThrow<double>(seg, "c");
  out.seg.h = requireNumberOrThrow<double>(seg, "h");
  out.seg.theta = requireNumberOrThrow<double>(seg, "theta");
  out.seg.psi = requireNumberOrThrow<double>(seg, "psi");
  out.seg.curvature = requireNumberOrThrow<double>(seg, "curvature");
  out.seg.alpha = requireNumberOrThrow<double>(seg, "alpha");
  out.seg.scale = requireNumberOrThrow<double>(seg, "scale");

  auto it = o.find("pos");
  if (it == o.end() || !it->value().is_array() || it->value().as_array().size() != 3) {
    throw ZException("Invalid LocalNeuroseg.pos: expected array[3]");
  }
  const auto& a = it->value().as_array();
  out.pos = {json::value_to<double>(a.at(0)), json::value_to<double>(a.at(1)), json::value_to<double>(a.at(2))};

  return out;
}

[[nodiscard]] json::object localNeurosegToJson(const LocalNeuroseg& v)
{
  json::object seg;
  seg["r1"] = v.seg.r1;
  seg["c"] = v.seg.c;
  seg["h"] = v.seg.h;
  seg["theta"] = v.seg.theta;
  seg["psi"] = v.seg.psi;
  seg["curvature"] = v.seg.curvature;
  seg["alpha"] = v.seg.alpha;
  seg["scale"] = v.seg.scale;

  json::array pos;
  pos.push_back(v.pos[0]);
  pos.push_back(v.pos[1]);
  pos.push_back(v.pos[2]);

  json::object o;
  o["seg"] = std::move(seg);
  o["pos"] = std::move(pos);
  return o;
}

[[nodiscard]] json::object traceConfigToJson(const TraceConfig& cfg)
{
  json::object o;
  o["minAutoScore"] = cfg.minAutoScore;
  o["minManualScore"] = cfg.minManualScore;
  o["minSeedScore"] = cfg.minSeedScore;
  o["min2dScore"] = cfg.min2dScore;

  o["refit"] = cfg.refit;
  o["spTest"] = cfg.spTest;
  o["crossoverTest"] = cfg.crossoverTest;
  o["tuneEnd"] = cfg.tuneEnd;
  o["edgePath"] = cfg.edgePath;
  o["enhanceMask"] = cfg.enhanceMask;

  o["seedMethod"] = cfg.seedMethod;
  o["recover"] = cfg.recover;

  o["chainScreenCount"] = cfg.chainScreenCount;
  o["maxEucDist"] = cfg.maxEucDist;
  return o;
}

} // namespace

ZBlockedAutoTraceSession::ZBlockedAutoTraceSession(QString sessionDir)
  : m_sessionDir(ensureDirPath(std::move(sessionDir)))
{
  if (m_sessionDir.isEmpty()) {
    throw ZException("Blocked auto trace: session directory is empty.");
  }
}

QString ZBlockedAutoTraceSession::manifestPath() const
{
  return QDir(m_sessionDir).absoluteFilePath(QStringLiteral("manifest.json"));
}

QString ZBlockedAutoTraceSession::blocksDirPath() const
{
  return QDir(m_sessionDir).absoluteFilePath(QStringLiteral("blocks"));
}

QString ZBlockedAutoTraceSession::rollingSwcPath() const
{
  return QDir(m_sessionDir).absoluteFilePath(QStringLiteral("result_tracing.swc"));
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceBlockSize& v)
{
  json::object o;
  o["core_x"] = v.coreX;
  o["core_y"] = v.coreY;
  o["core_z"] = v.coreZ;
  o["halo"] = v.halo;
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceDatasetShape& v)
{
  json::object o;
  o["width"] = v.width;
  o["height"] = v.height;
  o["depth"] = v.depth;
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceBlockId& v)
{
  json::object o;
  o["bx"] = v.bx;
  o["by"] = v.by;
  o["bz"] = v.bz;
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceSchedulerState& v)
{
  json::object o;
  o["next_linear_block_index"] = v.nextLinearBlockIndex;
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceManifest& v)
{
  json::object o;
  o["format_version"] = v.formatVersion;
  o["dataset_id"] = v.datasetId;
  o["channel"] = v.channel;
  o["time"] = v.time;

  json::array ratio;
  ratio.push_back(v.signalDownsampleRatio[0]);
  ratio.push_back(v.signalDownsampleRatio[1]);
  ratio.push_back(v.signalDownsampleRatio[2]);
  o["signal_downsample_ratio"] = std::move(ratio);
  o["z_scale"] = v.zToXYRatio;

  o["dataset_shape"] = toJson(v.datasetShape);
  o["block"] = toJson(v.block);
  o["threshold_mode"] = v.thresholdMode;
  o["subtract_constant"] = v.subtractConstant;
  o["trace_config"] = traceConfigToJson(v.traceConfig);
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTracePendingTask& v)
{
  json::object o;
  o["task_id"] = v.taskId;
  o["attach_swc_node_id"] = v.attachSwcNodeId;
  o["direction"] = std::string(traceDirectionToString(v.direction));
  o["end_locseg"] = localNeurosegToJson(v.endLocseg);
  o["reason"] = v.reason;
  o["suggested_block"] = toJson(v.suggestedBlock);
  return o;
}

json::object ZBlockedAutoTraceSession::toJson(const ZBlockedAutoTraceCommitInfo& v)
{
  json::object o;
  o["format_version"] = v.formatVersion;
  o["commit_id"] = v.commitId;
  o["block_id"] = toJson(v.blockId);
  o["did_seed_scan"] = v.didSeedScan;
  o["new_swc_nodes"] = v.newSwcNodes;
  o["pending_tasks"] = v.pendingTasks;
  o["created_utc_ms"] = static_cast<int64_t>(QDateTime::currentDateTimeUtc().toMSecsSinceEpoch());
  return o;
}

ZBlockedAutoTraceBlockSize ZBlockedAutoTraceSession::blockSizeFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceBlockSize out;
  out.coreX = requireNumberOrThrow<int64_t>(o, "core_x");
  out.coreY = requireNumberOrThrow<int64_t>(o, "core_y");
  out.coreZ = requireNumberOrThrow<int64_t>(o, "core_z");
  out.halo = requireNumberOrThrow<int64_t>(o, "halo");
  return out;
}

ZBlockedAutoTraceDatasetShape ZBlockedAutoTraceSession::datasetShapeFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceDatasetShape out;
  out.width = requireNumberOrThrow<int64_t>(o, "width");
  out.height = requireNumberOrThrow<int64_t>(o, "height");
  out.depth = requireNumberOrThrow<int64_t>(o, "depth");
  return out;
}

ZBlockedAutoTraceBlockId ZBlockedAutoTraceSession::blockIdFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceBlockId out;
  out.bx = requireNumberOrThrow<int64_t>(o, "bx");
  out.by = requireNumberOrThrow<int64_t>(o, "by");
  out.bz = requireNumberOrThrow<int64_t>(o, "bz");
  return out;
}

ZBlockedAutoTraceSchedulerState ZBlockedAutoTraceSession::schedulerFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceSchedulerState out;
  out.nextLinearBlockIndex = requireNumberOrThrow<uint64_t>(o, "next_linear_block_index");
  return out;
}

TraceConfig ZBlockedAutoTraceSession::traceConfigFromJsonOrThrow(const json::object& o)
{
  TraceConfig cfg;
  if (auto it = o.find("minAutoScore"); it != o.end()) {
    cfg.minAutoScore = json::value_to<double>(it->value());
  }
  if (auto it = o.find("minManualScore"); it != o.end()) {
    cfg.minManualScore = json::value_to<double>(it->value());
  }
  if (auto it = o.find("minSeedScore"); it != o.end()) {
    cfg.minSeedScore = json::value_to<double>(it->value());
  }
  if (auto it = o.find("min2dScore"); it != o.end()) {
    cfg.min2dScore = json::value_to<double>(it->value());
  }

  if (auto it = o.find("refit"); it != o.end()) {
    cfg.refit = json::value_to<bool>(it->value());
  }
  if (auto it = o.find("spTest"); it != o.end()) {
    cfg.spTest = json::value_to<bool>(it->value());
  }
  if (auto it = o.find("crossoverTest"); it != o.end()) {
    cfg.crossoverTest = json::value_to<bool>(it->value());
  }
  if (auto it = o.find("tuneEnd"); it != o.end()) {
    cfg.tuneEnd = json::value_to<bool>(it->value());
  }
  if (auto it = o.find("edgePath"); it != o.end()) {
    cfg.edgePath = json::value_to<bool>(it->value());
  }
  if (auto it = o.find("enhanceMask"); it != o.end()) {
    cfg.enhanceMask = json::value_to<bool>(it->value());
  }

  if (auto it = o.find("seedMethod"); it != o.end()) {
    cfg.seedMethod = json::value_to<int>(it->value());
  }
  if (auto it = o.find("recover"); it != o.end()) {
    cfg.recover = json::value_to<int>(it->value());
  }

  if (auto it = o.find("chainScreenCount"); it != o.end()) {
    cfg.chainScreenCount = json::value_to<int>(it->value());
  }
  if (auto it = o.find("maxEucDist"); it != o.end()) {
    cfg.maxEucDist = json::value_to<double>(it->value());
  }

  return cfg;
}

ZBlockedAutoTraceManifest ZBlockedAutoTraceSession::manifestFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceManifest out;
  out.formatVersion = requireNumberOrThrow<int>(o, "format_version");
  out.datasetId = requireStringOrThrow(o, "dataset_id");
  out.channel = requireNumberOrThrow<size_t>(o, "channel");
  out.time = requireNumberOrThrow<size_t>(o, "time");

  const json::array& ratio = requireArrayOrThrow(o, "signal_downsample_ratio");
  if (ratio.size() != 3) {
    throw ZException("Invalid signal_downsample_ratio: expected array[3].");
  }
  out.signalDownsampleRatio = {
    json::value_to<size_t>(ratio.at(0)),
    json::value_to<size_t>(ratio.at(1)),
    json::value_to<size_t>(ratio.at(2)),
  };
  if (out.signalDownsampleRatio[0] == 0 || out.signalDownsampleRatio[1] == 0 || out.signalDownsampleRatio[2] == 0) {
    throw ZException("Invalid signal_downsample_ratio: values must be > 0.");
  }
  out.zToXYRatio = requireNumberOrThrow<double>(o, "z_scale");
  if (!std::isfinite(out.zToXYRatio) || !(out.zToXYRatio > 0.0)) {
    throw ZException("Invalid z_scale: expected finite value > 0.");
  }

  out.datasetShape = datasetShapeFromJsonOrThrow(requireObjectOrThrow(o, "dataset_shape"));
  out.block = blockSizeFromJsonOrThrow(requireObjectOrThrow(o, "block"));

  if (auto it = o.find("threshold_mode"); it != o.end()) {
    if (!it->value().is_string()) {
      throw ZException(fmt::format("Invalid threshold_mode type: expected string, got {}", jsonTypeName(it->value())));
    }
    const auto& s = it->value().as_string();
    out.thresholdMode.assign(s.data(), s.size());
  } else {
    throw ZException("Missing threshold_mode.");
  }

  if (out.thresholdMode != "fixed" && out.thresholdMode != "auto") {
    throw ZException(fmt::format("Invalid threshold_mode: '{}'", out.thresholdMode));
  }

  out.subtractConstant = requireNumberOrThrow<double>(o, "subtract_constant");
  out.traceConfig = traceConfigFromJsonOrThrow(requireObjectOrThrow(o, "trace_config"));
  return out;
}

ZBlockedAutoTracePendingTask ZBlockedAutoTraceSession::pendingTaskFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTracePendingTask out;
  out.taskId = requireNumberOrThrow<uint64_t>(o, "task_id");
  out.attachSwcNodeId = requireNumberOrThrow<int64_t>(o, "attach_swc_node_id");

  const std::string dir = requireStringOrThrow(o, "direction");
  out.direction = traceDirectionFromStringOrThrow(dir);

  out.endLocseg = localNeurosegFromJsonOrThrow(requireObjectOrThrow(o, "end_locseg"));
  if (auto it = o.find("reason"); it != o.end() && it->value().is_string()) {
    const auto& s = it->value().as_string();
    out.reason.assign(s.data(), s.size());
  }

  if (auto ob = findObject(o, "suggested_block")) {
    out.suggestedBlock = blockIdFromJsonOrThrow(*ob);
  }
  return out;
}

ZBlockedAutoTraceCommitInfo ZBlockedAutoTraceSession::commitInfoFromJsonOrThrow(const json::object& o)
{
  ZBlockedAutoTraceCommitInfo out;
  out.formatVersion = requireNumberOrThrow<int>(o, "format_version");
  out.commitId = requireNumberOrThrow<uint64_t>(o, "commit_id");
  out.blockId = blockIdFromJsonOrThrow(requireObjectOrThrow(o, "block_id"));
  out.didSeedScan = requireBoolOrThrow(o, "did_seed_scan");
  out.newSwcNodes = requireNumberOrThrow<size_t>(o, "new_swc_nodes");
  out.pendingTasks = requireNumberOrThrow<size_t>(o, "pending_tasks");
  return out;
}

ZBlockedAutoTraceManifest ZBlockedAutoTraceSession::loadManifestOrThrow() const
{
  const QString path = manifestPath();
  if (!QFileInfo::exists(path)) {
    throw ZException(QStringLiteral("Blocked auto trace: missing manifest: %1").arg(path));
  }
  const json::object jo = loadJsonObject(path);
  ZBlockedAutoTraceManifest m = manifestFromJsonOrThrow(jo);
  if (m.formatVersion != kBlockedAutoTraceManifestFormatVersion) {
    throw ZException(fmt::format("Blocked auto trace: unsupported manifest format_version={}", m.formatVersion));
  }
  return m;
}

void ZBlockedAutoTraceSession::writeManifestAtomicOrThrow(const ZBlockedAutoTraceManifest& manifest) const
{
  const QString path = manifestPath();
  atomicWriteJsonObjectOrThrow(path, toJson(manifest));
}

void ZBlockedAutoTraceSession::ensureCreatedOrThrow(const ZBlockedAutoTraceManifest& manifest)
{
  const QDir root(m_sessionDir);
  if (!root.exists() && !QDir().mkpath(m_sessionDir)) {
    throw ZException(QStringLiteral("Blocked auto trace: can not create session directory: %1").arg(m_sessionDir));
  }

  const QString blocksPath = blocksDirPath();
  if (!QDir(blocksPath).exists() && !QDir().mkpath(blocksPath)) {
    throw ZException(QStringLiteral("Blocked auto trace: can not create blocks directory: %1").arg(blocksPath));
  }

  const QString manifestP = manifestPath();
  if (!QFileInfo::exists(manifestP)) {
    writeManifestAtomicOrThrow(manifest);
    return;
  }

  // Validate that the existing manifest matches the new request.
  const ZBlockedAutoTraceManifest existing = loadManifestOrThrow();
  if (existing.formatVersion != manifest.formatVersion) {
    throw ZException("Blocked auto trace: session manifest version mismatch.");
  }
  if (existing.datasetId != manifest.datasetId) {
    throw ZException("Blocked auto trace: dataset_id mismatch with existing session.");
  }
  if (existing.channel != manifest.channel || existing.time != manifest.time) {
    throw ZException("Blocked auto trace: channel/time mismatch with existing session.");
  }
  if (existing.signalDownsampleRatio != manifest.signalDownsampleRatio) {
    throw ZException("Blocked auto trace: signal_downsample_ratio mismatch with existing session.");
  }
  if (existing.zToXYRatio != manifest.zToXYRatio) {
    throw ZException("Blocked auto trace: z_scale mismatch with existing session.");
  }
  if (existing.datasetShape.width != manifest.datasetShape.width ||
      existing.datasetShape.height != manifest.datasetShape.height ||
      existing.datasetShape.depth != manifest.datasetShape.depth) {
    throw ZException("Blocked auto trace: dataset shape mismatch with existing session.");
  }
  if (existing.block.coreX != manifest.block.coreX || existing.block.coreY != manifest.block.coreY ||
      existing.block.coreZ != manifest.block.coreZ || existing.block.halo != manifest.block.halo) {
    throw ZException("Blocked auto trace: block size/halo mismatch with existing session.");
  }
  if (existing.thresholdMode != manifest.thresholdMode) {
    throw ZException("Blocked auto trace: threshold_mode mismatch with existing session.");
  }
  if (existing.subtractConstant != manifest.subtractConstant) {
    throw ZException("Blocked auto trace: subtract_constant mismatch with existing session.");
  }

  // TraceConfig affects core tracing thresholds and must remain stable for a session to be resumable.
  // We intentionally compare only the effective scalar fields persisted in the manifest (not levelOverrides).
  const TraceConfig& a = existing.traceConfig;
  const TraceConfig& b = manifest.traceConfig;
  if (a.minAutoScore != b.minAutoScore || a.minManualScore != b.minManualScore || a.minSeedScore != b.minSeedScore ||
      a.min2dScore != b.min2dScore || a.refit != b.refit || a.spTest != b.spTest ||
      a.crossoverTest != b.crossoverTest || a.tuneEnd != b.tuneEnd || a.edgePath != b.edgePath ||
      a.enhanceMask != b.enhanceMask || a.seedMethod != b.seedMethod || a.recover != b.recover ||
      a.chainScreenCount != b.chainScreenCount || a.maxEucDist != b.maxEucDist) {
    throw ZException("Blocked auto trace: trace_config mismatch with existing session.");
  }
}

QString ZBlockedAutoTraceSession::commitDirName(uint64_t commitId)
{
  return QStringLiteral("commit_%1").arg(static_cast<qulonglong>(commitId), 6, 10, QLatin1Char('0'));
}

QString ZBlockedAutoTraceSession::commitJsonName()
{
  return QStringLiteral("commit.json");
}

QString ZBlockedAutoTraceSession::frontierJsonName()
{
  return QStringLiteral("frontier.json");
}

QString ZBlockedAutoTraceSession::schedulerJsonName()
{
  return QStringLiteral("scheduler.json");
}

QString ZBlockedAutoTraceSession::swcDeltaName()
{
  return QStringLiteral("swc_delta.swc");
}

QString ZBlockedAutoTraceSession::swcFullName()
{
  return QStringLiteral("swc_full.swc");
}

QString ZBlockedAutoTraceSession::seedScannedBlocksName()
{
  return QStringLiteral("seed_scanned_blocks.json");
}

std::vector<ZBlockedAutoTraceSession::CommitDir> ZBlockedAutoTraceSession::listCommittedDirsSortedOrThrow() const
{
  const QString blocksPath = blocksDirPath();
  const QDir dir(blocksPath);
  if (!dir.exists()) {
    return {};
  }

  const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

  std::vector<CommitDir> commits;
  commits.reserve(static_cast<size_t>(entries.size()));

  for (const QString& name : entries) {
    if (!name.startsWith(QStringLiteral("commit_"))) {
      continue;
    }
    const QString suffix = name.mid(QStringLiteral("commit_").size());
    bool ok = false;
    const uint64_t id = suffix.toULongLong(&ok);
    if (!ok || id == 0) {
      continue;
    }

    const QString path = dir.absoluteFilePath(name);
    const QString commitJson = QDir(path).absoluteFilePath(commitJsonName());
    if (!QFileInfo::exists(commitJson)) {
      continue;
    }

    // Basic sanity check: commit.json parse must succeed.
    try {
      const json::object jo = loadJsonObject(commitJson);
      const ZBlockedAutoTraceCommitInfo info = commitInfoFromJsonOrThrow(jo);
      if (info.commitId != id) {
        continue;
      }
      if (info.formatVersion != kBlockedAutoTraceCommitFormatVersion) {
        continue;
      }
      commits.push_back(CommitDir{.id = id, .path = path, .info = info});
    }
    catch (...) {
      continue;
    }
  }

  std::sort(commits.begin(), commits.end(), [](const CommitDir& a, const CommitDir& b) {
    return a.id < b.id;
  });
  return commits;
}

uint64_t ZBlockedAutoTraceSession::latestCommittedIdOrZero() const
{
  const std::vector<CommitDir> commits = listCommittedDirsSortedOrThrow();
  if (commits.empty()) {
    return 0;
  }

  return commits.back().id;
}

void ZBlockedAutoTraceSession::writeSwcDeltaOrThrow(const QString& filePath,
                                                    const std::vector<ZBlockedAutoTraceSwcDeltaNode>& nodes)
{
  errno = 0;
  std::FILE* fp = std::fopen(filePath.toStdString().c_str(), "w");
  if (fp == nullptr) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to open SWC delta for writing: %1").arg(filePath),
                     ZException::Option::CheckErrno);
  }

  for (const auto& n : nodes) {
    std::fprintf(fp,
                 "%lld %lld %.17g %.17g %.17g %.17g %lld\n",
                 static_cast<long long>(n.id),
                 static_cast<long long>(n.type),
                 n.x,
                 n.y,
                 n.z,
                 n.radius,
                 static_cast<long long>(n.parentId));
  }

  errno = 0;
  if (std::fclose(fp) != 0) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to close SWC delta: %1").arg(filePath),
                     ZException::Option::CheckErrno);
  }
}

std::vector<ZBlockedAutoTraceBlockId> ZBlockedAutoTraceSession::readSeedScannedBlocksOrThrow(const QString& filePath)
{
  const json::object jo = loadJsonObject(filePath);
  auto it = jo.find("blocks");
  if (it == jo.end() || !it->value().is_array()) {
    throw ZException(QStringLiteral("Blocked auto trace: invalid seed_scanned_blocks.json: %1").arg(filePath));
  }

  const auto& arr = it->value().as_array();
  std::vector<ZBlockedAutoTraceBlockId> out;
  out.reserve(arr.size());
  for (const auto& value : arr) {
    if (!value.is_object()) {
      throw ZException(QStringLiteral("Blocked auto trace: invalid seed_scanned_blocks entry: %1").arg(filePath));
    }
    out.push_back(blockIdFromJsonOrThrow(value.as_object()));
  }
  return out;
}

void ZBlockedAutoTraceSession::writeSeedScannedBlocksOrThrow(const QString& filePath,
                                                             const std::vector<ZBlockedAutoTraceBlockId>& blocks)
{
  json::object jo;
  json::array arr;
  arr.reserve(blocks.size());
  for (const auto& block : blocks) {
    arr.push_back(toJson(block));
  }
  jo["blocks"] = std::move(arr);
  atomicWriteJsonObjectOrThrow(filePath, jo);
}

namespace {

void copyFileAtomicOrThrow(const QString& sourcePath, const QString& finalPath)
{
  QFile source(sourcePath);
  if (!source.open(QIODevice::ReadOnly)) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to open source file: %1").arg(sourcePath));
  }

  QSaveFile dest(finalPath);
  if (!dest.open(QIODevice::WriteOnly)) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to open destination file: %1").arg(finalPath));
  }

  std::vector<char> buffer(1 << 20);
  while (true) {
    const qint64 nread = source.read(buffer.data(), static_cast<qint64>(buffer.size()));
    if (nread < 0) {
      throw ZException(QStringLiteral("Blocked auto trace: failed while reading source file: %1").arg(sourcePath));
    }
    if (nread == 0) {
      break;
    }
    const qint64 nwritten = dest.write(buffer.data(), nread);
    if (nwritten != nread) {
      throw ZException(QStringLiteral("Blocked auto trace: failed while writing destination file: %1").arg(finalPath));
    }
  }

  if (!dest.commit()) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to commit destination file: %1").arg(finalPath));
  }
}

} // namespace

void ZBlockedAutoTraceSession::rebuildNodeByIdOrThrow(ZSwc& swc,
                                                      std::unordered_map<int64_t, ZSwc::SwcTreeNode>& nodeById)
{
  nodeById.clear();
  nodeById.reserve(swc.size() * 2 + 1);
  for (auto it = swc.begin(); it != swc.end(); ++it) {
    if (it->id <= 0) {
      throw ZException(fmt::format("Blocked auto trace: invalid SWC node id in snapshot: {}", it->id));
    }
    auto [insertedIt, inserted] = nodeById.emplace(it->id, it);
    if (!inserted) {
      throw ZException(fmt::format("Blocked auto trace: duplicate SWC node id in snapshot: {}", insertedIt->first));
    }
  }
}

void ZBlockedAutoTraceSession::updateRollingSwcFromCommitOrThrow(uint64_t commitId) const
{
  if (commitId == 0) {
    throw ZException("Blocked auto trace: rolling SWC update requires commitId > 0.");
  }

  const QString commitPath = QDir(blocksDirPath()).absoluteFilePath(commitDirName(commitId));
  const QString swcFullPath = QDir(commitPath).absoluteFilePath(swcFullName());
  copyFileAtomicOrThrow(swcFullPath, rollingSwcPath());
}

ZBlockedAutoTraceLoadedState ZBlockedAutoTraceSession::loadLatestOrEmptyOrThrow() const
{
  const ZBlockedAutoTraceManifest manifest = loadManifestOrThrow();
  const std::vector<CommitDir> commits = listCommittedDirsSortedOrThrow();

  if (commits.empty()) {
    ZBlockedAutoTraceLoadedState out;
    out.manifest = manifest;
    out.scheduler.nextLinearBlockIndex = 0;
    return out;
  }

  auto loadFrontierAndSchedulerOrThrow = [&](const CommitDir& commitDir, ZBlockedAutoTraceLoadedState& state) {
    const QDir dir(commitDir.path);
    const QString frontierPath = dir.absoluteFilePath(frontierJsonName());
    const json::object frontierObj = loadJsonObject(frontierPath);
    auto it = frontierObj.find("tasks");
    if (it == frontierObj.end() || !it->value().is_array()) {
      throw ZException("Blocked auto trace: invalid frontier.json (missing tasks array).");
    }
    const auto& arr = it->value().as_array();
    state.frontier.clear();
    state.frontier.reserve(arr.size());
    for (const auto& value : arr) {
      if (!value.is_object()) {
        throw ZException("Blocked auto trace: invalid frontier.json task entry (expected object).");
      }
      state.frontier.push_back(pendingTaskFromJsonOrThrow(value.as_object()));
    }

    const QString schedulerPath = dir.absoluteFilePath(schedulerJsonName());
    const json::object schedulerObj = loadJsonObject(schedulerPath);
    state.scheduler = schedulerFromJsonOrThrow(schedulerObj);
  };

  auto loadSelfContainedCommitOrThrow = [&](const CommitDir& commitDir) {
    CHECK(commitDir.info.formatVersion == kBlockedAutoTraceCommitFormatVersion);

    ZBlockedAutoTraceLoadedState state;
    state.manifest = manifest;
    state.commitId = commitDir.id;

    const QDir dir(commitDir.path);
    const QString swcFullPath = dir.absoluteFilePath(swcFullName());
    const QString seedScannedPath = dir.absoluteFilePath(seedScannedBlocksName());
    if (!QFileInfo::exists(swcFullPath) || !QFileInfo::exists(seedScannedPath) ||
        !QFileInfo::exists(dir.absoluteFilePath(frontierJsonName())) ||
        !QFileInfo::exists(dir.absoluteFilePath(schedulerJsonName()))) {
      throw ZException(
        QStringLiteral("Blocked auto trace: missing files in self-contained commit: %1").arg(commitDir.path));
    }

    state.swc.load(swcFullPath);
    rebuildNodeByIdOrThrow(state.swc, state.nodeById);
    state.seedScannedBlocks = readSeedScannedBlocksOrThrow(seedScannedPath);
    loadFrontierAndSchedulerOrThrow(commitDir, state);
    return state;
  };

  bool sawCommittedDir = false;
  for (auto it = commits.rbegin(); it != commits.rend(); ++it) {
    sawCommittedDir = true;
    try {
      return loadSelfContainedCommitOrThrow(*it);
    }
    catch (const std::exception& e) {
      LOG(WARNING) << fmt::format("Blocked auto trace: ignoring broken commit {} during resume: {}", it->id, e.what());
    }
    catch (...) {
      LOG(WARNING) << fmt::format("Blocked auto trace: ignoring broken commit {} during resume.", it->id);
    }
  }

  CHECK(sawCommittedDir);
  throw ZException("Blocked auto trace: found commit directories, but none were loadable.");
}

void ZBlockedAutoTraceSession::writeCommitOrThrow(const ZBlockedAutoTraceCommitWrite& commit,
                                                  const ZSwc& swcSnapshot,
                                                  const std::vector<ZBlockedAutoTraceBlockId>& seedScannedBlocks)
{
  if (commit.info.commitId == 0) {
    throw ZException("Blocked auto trace: commitId must be > 0.");
  }
  if (commit.info.formatVersion != kBlockedAutoTraceCommitFormatVersion) {
    throw ZException("Blocked auto trace: unsupported commit formatVersion.");
  }

  const uint64_t expected = latestCommittedIdOrZero() + 1;
  if (commit.info.commitId != expected) {
    throw ZException(
      fmt::format("Blocked auto trace: refusing to write commit {} (expected {}).", commit.info.commitId, expected));
  }

  const QString blocksPath = blocksDirPath();
  const QDir blocksDir(blocksPath);
  if (!blocksDir.exists()) {
    throw ZException(QStringLiteral("Blocked auto trace: missing blocks dir: %1").arg(blocksPath));
  }

  const QString finalName = commitDirName(commit.info.commitId);
  const QString finalPath = blocksDir.absoluteFilePath(finalName);
  if (QFileInfo::exists(finalPath)) {
    throw ZException(QStringLiteral("Blocked auto trace: commit dir already exists: %1").arg(finalPath));
  }

  const QString stagingName =
    QStringLiteral(".staging_") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral("_") + finalName;
  const QString stagingPath = blocksDir.absoluteFilePath(stagingName);

  if (!QDir().mkpath(stagingPath)) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to create staging dir: %1").arg(stagingPath));
  }

  auto stagingGuard = folly::makeGuard([&]() {
    (void)QDir(stagingPath).removeRecursively();
  });

  // Write payload files first.
  {
    const QString deltaPath = QDir(stagingPath).absoluteFilePath(swcDeltaName());
    writeSwcDeltaOrThrow(deltaPath, commit.swcDeltaNodes);
  }
  {
    const QString swcFullPath = QDir(stagingPath).absoluteFilePath(swcFullName());
    swcSnapshot.save(swcFullPath);
  }
  {
    const QString seedScannedPath = QDir(stagingPath).absoluteFilePath(seedScannedBlocksName());
    writeSeedScannedBlocksOrThrow(seedScannedPath, seedScannedBlocks);
  }
  {
    json::object frontierObj;
    json::array tasks;
    tasks.reserve(commit.frontier.size());
    for (const auto& t : commit.frontier) {
      tasks.push_back(toJson(t));
    }
    frontierObj["tasks"] = std::move(tasks);
    atomicWriteJsonObjectOrThrow(QDir(stagingPath).absoluteFilePath(frontierJsonName()), frontierObj);
  }
  {
    atomicWriteJsonObjectOrThrow(QDir(stagingPath).absoluteFilePath(schedulerJsonName()), toJson(commit.scheduler));
  }

  // Write commit marker last.
  {
    atomicWriteJsonObjectOrThrow(QDir(stagingPath).absoluteFilePath(commitJsonName()), toJson(commit.info));
  }

  // Atomically rename staging -> final.
  if (!QDir(blocksPath).rename(stagingName, finalName)) {
    throw ZException(QStringLiteral("Blocked auto trace: failed to commit staging dir.\nStaging: %1\nFinal: %2")
                       .arg(stagingPath, finalPath));
  }

  stagingGuard.dismiss();
}

} // namespace nim
