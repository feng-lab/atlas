#include "zneutubetrace.h"

#include "zneutubelocsegchain.h"
#include "zneutubelocsegchaincircle.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubeswcloaderlegacy.h"
#include "zneutubetraceauto.h"
#include "zneutubetraceconfig.h"
#include "zneutubetraceconnect.h"
#include "zneutubetraceworkspace.h"
#include "zneutubetraceswclabelstack.h"
#include "zneutubeswcwriter.h"

#include "zimg.h"
#include "zlog.h"

#include <QDir>
#include <QFileInfo>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim::neutube {

namespace {

constexpr double TzPiOver4LegacyLike = 0.78539816339744830961566084582;

[[nodiscard]] std::string resolveTraceConfigPathLegacyLike(const std::string& traceConfigPath,
                                                           const std::string& jsonDirPath)
{
  if (!traceConfigPath.empty()) {
    return traceConfigPath;
  }
  if (jsonDirPath.empty()) {
    return {};
  }
  return QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
}

[[nodiscard]] bool fileExists(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  const QFileInfo fi(QString::fromStdString(path));
  return fi.exists() && fi.isFile();
}

[[nodiscard]] bool isSwcFilePathLegacyLike(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  const QFileInfo fi(QString::fromStdString(path));
  return fi.suffix().compare("swc", Qt::CaseInsensitive) == 0;
}

[[nodiscard]] int runSeededTraceLegacyLike(const std::string& signalPath,
                                           const std::string& outputPath,
                                           const std::array<int, 3>& position,
                                           int level,
                                           const std::string& traceConfigPath,
                                           const std::string& jsonDirPath)
{
  nim::ZImg signal;
  try {
    signal.load(QString::fromStdString(signalPath));
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

  const std::string resolvedConfigPath = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);

  TraceConfig baseCfg;
  if (!resolvedConfigPath.empty()) {
    const bool ok = loadTraceConfigLegacyLike(resolvedConfigPath, &baseCfg);
    if (!ok) {
      LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfigPath;
    }
  } else {
    LOG(WARNING) << "Tracing configuration skipped: no trace config path and no json dir available.";
    baseCfg = TraceConfig{};
  }

  TraceConfig cfg = baseCfg;
  if (level > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(baseCfg, level)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, &cfg);
    }
  }

  // Port of ZNeuronTracer::trace(double x, double y, double z).
  TraceWorkspace tw;
  locsegChainDefaultTraceWorkspaceLegacyLike(&tw, &signal);

  // Port of ZNeuronTracer::configure() effects on the trace workspace.
  tw.refit = cfg.refit;
  tw.tuneEnd = cfg.tuneEnd;

  // Port of ZNeuronTracer::prepareTraceScoreThreshold(TRACING_INTERACTIVE).
  if (signal.depth() == 1) {
    tw.minScore = cfg.min2dScore;
  } else {
    tw.minScore = cfg.minManualScore;
  }

  traceWorkspaceInitTraceMaskLegacyLike(&tw, signal, false);

  std::array<double, 3> pos{};
  pos[0] = static_cast<double>(position[0]);
  pos[1] = static_cast<double>(position[1]);
  pos[2] = static_cast<double>(position[2]);

  LocalNeuroseg seedLocseg;
  seedLocseg.seg.r1 = 3.0;
  seedLocseg.seg.c = 0.0;
  seedLocseg.seg.h = 11.0;
  seedLocseg.seg.theta = TzPiOver4LegacyLike;
  seedLocseg.seg.psi = 0.0;
  seedLocseg.seg.curvature = 0.0;
  seedLocseg.seg.alpha = 0.0;
  seedLocseg.seg.scale = 1.0;
  setNeurosegPositionLegacyLike(&seedLocseg, pos, NeuroposReferenceLegacyLike::Center);

  (void)localNeurosegOptimizeWLegacyLike(&seedLocseg, signal, 1.0, 1, &tw.fitWorkspace);

  TraceRecord seedTr;
  traceRecordReset(&seedTr);
  traceRecordSetFixPoint(&seedTr, 0.0);
  traceRecordSetDirection(&seedTr, TraceDirection::BothDir);

  LocsegChain chain;
  LocsegNode node;
  node.locseg = seedLocseg;
  node.tr = seedTr;
  (void)chain.addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

  traceWorkspaceSetTraceStatusLegacyLike(&tw, TraceStatus::Normal, TraceStatus::Normal);
  traceLocsegLegacyLike(signal, 1.0, &chain, &tw);
  (void)locsegChainRemoveOverlapEndsLegacyLike(&chain);
  locsegChainRemoveTurnEndsLegacyLike(&chain, 1.0);

  const std::vector<Geo3dCircle> circles = locsegChainToGeo3dCircleArrayLegacyLike(chain);

  if (circles.empty()) {
    return 1;
  }

  int start = 0;
  int end = static_cast<int>(circles.size());

  if (traceWorkspaceMaskValueLegacyLike(tw, circles.front().center) > 0) {
    for (int i = 1; i < static_cast<int>(circles.size()); ++i) {
      start = i - 1;
      if (traceWorkspaceMaskValueLegacyLike(tw, circles[static_cast<size_t>(i)].center) == 0) {
        break;
      }
    }
  }

  if (circles.size() > 1) {
    if (traceWorkspaceMaskValueLegacyLike(tw, circles.back().center) > 0) {
      for (int i = static_cast<int>(circles.size()) - 2; i >= 0; --i) {
        end = i + 2;
        if (traceWorkspaceMaskValueLegacyLike(tw, circles[static_cast<size_t>(i)].center) == 0) {
          break;
        }
      }
    }
  }

  if (start >= end) {
    return 1;
  }

  nim::ZSwc swc;
  nim::ZSwc::SwcTreeNode parent = swc.end();

  for (int i = start; i < end; ++i) {
    const Geo3dCircle& circle = circles[static_cast<size_t>(i)];
    nim::SwcNode swcNode(/*id*/ 1,
                         /*type*/ 0,
                         circle.center[0],
                         circle.center[1],
                         circle.center[2],
                         circle.radius,
                         /*parentID*/ -1);

    if (i == start) {
      parent = swc.appendRoot(swcNode);
    } else {
      parent = swc.appendChild(parent, swcNode);
    }
  }

  writeSwcLegacyNeuTu(&swc, outputPath);
  return 0;
}

[[nodiscard]] int runSeededTraceWithHostSwcLegacyLike(const std::string& signalPath,
                                                      const std::string& hostSwcPath,
                                                      const std::string& outputPath,
                                                      const std::array<int, 3>& position,
                                                      int level,
                                                      const std::string& traceConfigPath,
                                                      const std::string& jsonDirPath)
{
  if (!isSwcFilePathLegacyLike(hostSwcPath)) {
    return runSeededTraceLegacyLike(signalPath, outputPath, position, level, traceConfigPath, jsonDirPath);
  }

  nim::ZImg signal;
  try {
    signal.load(QString::fromStdString(signalPath));
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

  const std::string resolvedConfigPath = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);

  TraceConfig baseCfg;
  if (!resolvedConfigPath.empty()) {
    const bool ok = loadTraceConfigLegacyLike(resolvedConfigPath, &baseCfg);
    if (!ok) {
      LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfigPath;
    }
  } else {
    LOG(WARNING) << "Tracing configuration skipped: no trace config path and no json dir available.";
    baseCfg = TraceConfig{};
  }

  TraceConfig cfg = baseCfg;
  if (level > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(baseCfg, level)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, &cfg);
    }
  }

  nim::ZSwc hostSwc;
  std::string swcError;
  if (!loadSwcLegacyOrder(hostSwcPath, &hostSwc, &swcError)) {
    LOG(ERROR) << "Failed to read host SWC: " << hostSwcPath << " (" << swcError << ")";
    return 1;
  }

  std::vector<nim::ZSwc::SwcTreeNode> hostRoots;
  for (auto it = hostSwc.beginRoot(); it != hostSwc.endRoot(); ++it) {
    hostRoots.emplace_back(nim::ZSwc::SwcTreeNode(it));
  }

  TraceWorkspace tw;
  locsegChainDefaultTraceWorkspaceLegacyLike(&tw, &signal);

  tw.refit = cfg.refit;
  tw.tuneEnd = cfg.tuneEnd;

  if (signal.depth() == 1) {
    tw.minScore = cfg.min2dScore;
  } else {
    tw.minScore = cfg.minManualScore;
  }

  const ZImgInfo maskInfo(signal.width(), signal.height(), signal.depth(), 1, 1, 1, VoxelFormat::Unsigned);
  tw.traceMask = std::make_unique<ZImg>(maskInfo);
  tw.traceMask->fill(0);
  labelSwcIntoMaskLegacyLike(hostSwc, tw.traceMask.get(), /*zScale*/ 1.0, /*value*/ 255);

  std::array<double, 3> pos{};
  pos[0] = static_cast<double>(position[0]);
  pos[1] = static_cast<double>(position[1]);
  pos[2] = static_cast<double>(position[2]);

  LocalNeuroseg seedLocseg;
  seedLocseg.seg.r1 = 3.0;
  seedLocseg.seg.c = 0.0;
  seedLocseg.seg.h = 11.0;
  seedLocseg.seg.theta = TzPiOver4LegacyLike;
  seedLocseg.seg.psi = 0.0;
  seedLocseg.seg.curvature = 0.0;
  seedLocseg.seg.alpha = 0.0;
  seedLocseg.seg.scale = 1.0;
  setNeurosegPositionLegacyLike(&seedLocseg, pos, NeuroposReferenceLegacyLike::Center);

  (void)localNeurosegOptimizeWLegacyLike(&seedLocseg, signal, 1.0, 1, &tw.fitWorkspace);

  TraceRecord seedTr;
  traceRecordReset(&seedTr);
  traceRecordSetFixPoint(&seedTr, 0.0);
  traceRecordSetDirection(&seedTr, TraceDirection::BothDir);

  LocsegChain chain;
  LocsegNode node;
  node.locseg = seedLocseg;
  node.tr = seedTr;
  (void)chain.addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

  traceWorkspaceSetTraceStatusLegacyLike(&tw, TraceStatus::Normal, TraceStatus::Normal);
  traceLocsegLegacyLike(signal, 1.0, &chain, &tw);
  (void)locsegChainRemoveOverlapEndsLegacyLike(&chain);
  locsegChainRemoveTurnEndsLegacyLike(&chain, 1.0);

  const std::vector<Geo3dCircle> circles = locsegChainToGeo3dCircleArrayLegacyLike(chain);
  if (circles.empty()) {
    writeSwcLegacyNeuTu(&hostSwc, outputPath);
    return 0;
  }

  int start = 0;
  int end = static_cast<int>(circles.size());

  if (traceWorkspaceMaskValueLegacyLike(tw, circles.front().center) > 0) {
    for (int i = 1; i < static_cast<int>(circles.size()); ++i) {
      start = i - 1;
      if (traceWorkspaceMaskValueLegacyLike(tw, circles[static_cast<size_t>(i)].center) == 0) {
        break;
      }
    }
  }

  if (circles.size() > 1) {
    if (traceWorkspaceMaskValueLegacyLike(tw, circles.back().center) > 0) {
      for (int i = static_cast<int>(circles.size()) - 2; i >= 0; --i) {
        end = i + 2;
        if (traceWorkspaceMaskValueLegacyLike(tw, circles[static_cast<size_t>(i)].center) == 0) {
          break;
        }
      }
    }
  }

  if (start >= end || (end - start) <= 1) {
    writeSwcLegacyNeuTu(&hostSwc, outputPath);
    return 0;
  }

  nim::ZSwc::SwcTreeNode parent = hostSwc.end();
  nim::ZSwc::SwcTreeNode branchRoot = hostSwc.end();

  for (int i = start; i < end; ++i) {
    const Geo3dCircle& circle = circles[static_cast<size_t>(i)];
    nim::SwcNode swcNode(/*id*/ 1,
                         /*type*/ 0,
                         circle.center[0],
                         circle.center[1],
                         circle.center[2],
                         circle.radius,
                         /*parentID*/ -1);

    if (i == start) {
      parent = hostSwc.appendRoot(swcNode);
      branchRoot = parent;
    } else {
      parent = hostSwc.appendChild(parent, swcNode);
    }
  }

  connectBranchToHostLegacyLike(&hostSwc, hostRoots, branchRoot, signal);

  writeSwcLegacyNeuTu(&hostSwc, outputPath);
  return 0;
}

} // namespace

int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool verbose)
{
  if (input.empty()) {
    LOG(INFO) << "No input specified. Abort.";
    return 1;
  }

  if (input[0].empty()) {
    LOG(INFO) << "No input data specified. Abort.";
    return 1;
  }

  if (outputPath.empty()) {
    LOG(INFO) << "No output specified. Abort.";
    return 1;
  }

  if (!position.has_value()) {
    nim::ZImg signal;
    try {
      signal.load(QString::fromStdString(input[0]));
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Failed to read input image: " << input[0] << " (" << e.what() << ")";
      return 1;
    }

    if (signal.isEmpty()) {
      LOG(ERROR) << "Failed to read input image (empty): " << input[0];
      return 1;
    }

    const std::string resolvedConfigPath = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);

    TraceConfig baseCfg;
    if (!resolvedConfigPath.empty()) {
      const bool ok = loadTraceConfigLegacyLike(resolvedConfigPath, &baseCfg);
      if (!ok) {
        LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfigPath;
      }
    } else {
      LOG(WARNING) << "Tracing configuration skipped: no trace config path and no json dir available.";
      baseCfg = TraceConfig{};
    }

    TraceConfig cfg = baseCfg;
    if (level > 0) {
      if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(baseCfg, level)) {
        applyTraceConfigOverridesLegacyLike(*levelOverride, &cfg);
      }
    }

    std::unique_ptr<ZSwc> tree = traceNeuronAutoLegacyLike(std::move(signal), cfg, diagnosis, verbose, nullptr);
    if (tree) {
      writeSwcLegacyNeuTu(tree.get(), outputPath);
      return 0;
    }

    LOG(WARNING) << "WARNING: No result generated.";
    return 1;
  }

  const std::string resolvedConfig = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);
  if (!resolvedConfig.empty() && !fileExists(resolvedConfig)) {
    LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfig;
  }

  const std::string swcPath = (input.size() > 1) ? input[1] : std::string{};
  const int rc = (swcPath.empty() || !isSwcFilePathLegacyLike(swcPath))
                   ? runSeededTraceLegacyLike(input[0], outputPath, *position, level, traceConfigPath, jsonDirPath)
                   : runSeededTraceWithHostSwcLegacyLike(input[0],
                                                         swcPath,
                                                         outputPath,
                                                         *position,
                                                         level,
                                                         traceConfigPath,
                                                         jsonDirPath);
  if (rc != 0) {
    LOG(WARNING) << "WARNING: No result generated.";
  }
  return rc;
}

} // namespace nim::neutube
