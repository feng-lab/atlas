#include "zneutubetraceinteractive.h"

#include "zneutubelocsegchain.h"
#include "zneutubelocsegchaincircle.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubetraceconnect.h"
#include "zneutubetraceworkspace.h"
#include "zneutubetraceswclabelstack.h"
#include "zneutubetracezscale.h"

#include "zsparsevoxelmask.h"
#include "zswcgeometrymaskvolume.h"
#include "zswcspatialindex.h"

#include "zcancellation.h"
#include "zlog.h"
#include "zswcops.h"

#include <gflags/gflags.h>

#include <array>
#include <cmath>
#include <utility>
#include <vector>

DEFINE_bool(atlas_trace_use_swc_geometry_mask,
            true,
            "Use a continuous-geometry SWC spatial index for host-SWC mask queries when available. "
            "Disable to force legacy voxel-mask semantics (useful for parity tests).");

namespace nim {

namespace {

constexpr double TzPiOver4LegacyLike = 0.78539816339744830961566084582;

struct TraceCirclesResult
{
  std::vector<Geo3dCircle> circles;
  TraceWorkspace tw;
};

template<typename TSignal>
[[nodiscard]] TraceCirclesResult traceSeedToCirclesLegacyLikeImpl(const TSignal& signal,
                                                                  const std::array<double, 3>& position,
                                                                  const TraceConfig& cfg,
                                                                  double zToXYRatio,
                                                                  const folly::CancellationToken& cancellationToken)
{
  TraceCirclesResult res;
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  locsegChainDefaultTraceWorkspaceLegacyLike(res.tw, signal);
  res.tw.cancellationToken = cancellationToken;
  res.tw.refit = cfg.refit;
  res.tw.tuneEnd = cfg.tuneEnd;
  res.tw.resolution = traceResolutionFromZToXYRatioLegacyLike(zToXYRatio);

  if (signal.depth() == 1) {
    res.tw.minScore = cfg.min2dScore;
  } else {
    res.tw.minScore = cfg.minManualScore;
  }

  LocalNeuroseg seedLocseg;
  seedLocseg.seg.r1 = 3.0;
  seedLocseg.seg.c = 0.0;
  seedLocseg.seg.h = 11.0;
  seedLocseg.seg.theta = TzPiOver4LegacyLike;
  seedLocseg.seg.psi = 0.0;
  seedLocseg.seg.curvature = 0.0;
  seedLocseg.seg.alpha = 0.0;
  seedLocseg.seg.scale = 1.0;
  setNeurosegPositionLegacyLike(seedLocseg,
                                {position[0], position[1], position[2] * zToXYRatio},
                                NeuroposReferenceLegacyLike::Center);

  VLOG(1) << fmt::format("Seed trace: optimize seed at ({:.3f}, {:.3f}, {:.3f})",
                         position[0],
                         position[1],
                         position[2]);

  maybeCancel(res.tw.cancellationToken);
  const double seedScore =
    localNeurosegOptimizeWLegacyLike(seedLocseg, signal, zToXYRatio, /*option*/ 1, res.tw.fitWorkspace);
  maybeCancel(res.tw.cancellationToken);

  VLOG(1) << fmt::format("Seed trace: seed optimize score={:.6f} (minScore={:.6f})", seedScore, res.tw.minScore);

  TraceRecord seedTr;
  traceRecordReset(seedTr);
  traceRecordSetFixPoint(seedTr, 0.0);
  traceRecordSetDirection(seedTr, TraceDirection::BothDir);

  LocsegChain chain;
  LocsegNode node;
  node.locseg = seedLocseg;
  node.tr = seedTr;
  (void)chain.addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

  traceWorkspaceSetTraceStatusLegacyLike(res.tw, TraceStatus::Normal, TraceStatus::Normal);

  VLOG(1) << "Seed trace: tracing chain ...";
  traceLocsegLegacyLike(signal, zToXYRatio, chain, res.tw);
  maybeCancel(res.tw.cancellationToken);
  VLOG(1) << fmt::format("Seed trace: chain trace done (len={}, status=[{},{}])",
                         chain.length(),
                         static_cast<int>(res.tw.traceStatus[0]),
                         static_cast<int>(res.tw.traceStatus[1]));

  (void)locsegChainRemoveOverlapEndsLegacyLike(chain);
  locsegChainRemoveTurnEndsLegacyLike(chain, 1.0);

  res.circles = locsegChainToGeo3dCircleArrayZLegacyLike(chain, zToXYRatio);
  return res;
}

[[nodiscard]] ZImg traceSignalViewLegacyLike(const ZImg& signal, size_t c, size_t t)
{
  if (signal.isEmpty()) {
    return {};
  }

  const ZImgInfo info = signal.info();
  if (c >= info.numChannels || t >= info.numTimes) {
    throw ZException(
      fmt::format("Seed trace: invalid channel/time selection (c={}, t={}) for signal <{}>.", c, t, info));
  }

  return signal.createView(static_cast<index_t>(c), static_cast<index_t>(t));
}

// Port of the start/end trimming used by the seeded-trace codepaths.
// Returns [start,end) indices into `circles`.
[[nodiscard]] std::pair<int, int> trimmedCircleIndexRangeLegacyLike(const TraceWorkspace& tw,
                                                                    const std::vector<Geo3dCircle>& circles)
{
  if (circles.empty()) {
    return {0, 0};
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

  return {start, end};
}

template<typename TSignal>
[[nodiscard]] SeedTraceResult traceSeedNewSwcFromSignalViewLegacyLike(const TSignal& signal,
                                                                      const std::array<double, 3>& position,
                                                                      const TraceConfig& cfg,
                                                                      double zToXYRatio,
                                                                      folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    return {};
  }

  LOG(INFO) << fmt::format("Seed trace (new SWC): seed=({:.3f}, {:.3f}, {:.3f})",
                           position[0],
                           position[1],
                           position[2]);

  TraceCirclesResult tr = traceSeedToCirclesLegacyLikeImpl(signal, position, cfg, zToXYRatio, cancellationToken);
  if (tr.circles.empty()) {
    VLOG(1) << "Seed trace: no circles produced.";
    return {};
  }

  maybeCancel(cancellationToken);
  const auto [start, end] = trimmedCircleIndexRangeLegacyLike(tr.tw, tr.circles);
  if (start >= end) {
    VLOG(1) << "Seed trace: circle range empty after trimming.";
    return {};
  }

  auto swc = std::make_unique<ZSwc>();
  ZSwc::SwcTreeNode parent = swc->end();

  for (int i = start; i < end; ++i) {
    const Geo3dCircle& circle = tr.circles[static_cast<size_t>(i)];
    SwcNode swcNode(/*id*/ 1,
                    /*type*/ 0,
                    circle.center[0],
                    circle.center[1],
                    circle.center[2],
                    circle.radius,
                    /*parentID*/ -1);
    swcNode.selected = true;

    if (i == start) {
      parent = swc->appendRoot(swcNode);
    } else {
      parent = swc->appendChild(parent, swcNode);
    }
  }

  // `ZNeuronConstructor::reconstruct` resorts IDs before returning its ZSwcTree.
  resortId(*swc);

  SeedTraceResult res;
  res.newNodes = static_cast<size_t>(end - start);
  res.swc = std::move(swc);
  LOG(INFO) << fmt::format("Seed trace (new SWC): done (newNodes={})", res.newNodes);
  return res;
}

template<typename TSignal>
[[nodiscard]] SeedTraceResult traceSeedIntoHostSwcFromSignalViewLegacyLike(const TSignal& signal,
                                                                           const ZSwc& hostSwc,
                                                                           const std::array<double, 3>& position,
                                                                           const TraceConfig& cfg,
                                                                           double zToXYRatio,
                                                                           folly::CancellationToken cancellationToken)
{
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  auto outSwc = std::make_unique<ZSwc>(hostSwc);
  for (auto& tn : *outSwc) {
    tn.selected = false;
  }

  LOG(INFO) << fmt::format("Seed trace (attach): seed=({:.3f}, {:.3f}, {:.3f})", position[0], position[1], position[2]);

  std::vector<ZSwc::SwcTreeNode> hostRoots;
  hostRoots.reserve(outSwc->numRoots());
  for (auto it = outSwc->beginRoot(); it != outSwc->endRoot(); ++it) {
    hostRoots.emplace_back(ZSwc::SwcTreeNode(it));
  }

  if (signal.isEmpty()) {
    return {.swc = std::move(outSwc), .newNodes = 0};
  }

  // This variant follows the CLI "host SWC provided" behavior: the host is labeled into a mask so that the traced
  // branch can be trimmed and then connected to the host structure.
  TraceCirclesResult tr;
  locsegChainDefaultTraceWorkspaceLegacyLike(tr.tw, signal);
  tr.tw.cancellationToken = cancellationToken;
  tr.tw.refit = cfg.refit;
  tr.tw.tuneEnd = cfg.tuneEnd;
  tr.tw.resolution = traceResolutionFromZToXYRatioLegacyLike(zToXYRatio);

  if (signal.depth() == 1) {
    tr.tw.minScore = cfg.min2dScore;
  } else {
    tr.tw.minScore = cfg.minManualScore;
  }

  if (FLAGS_atlas_trace_use_swc_geometry_mask) {
    auto idx = std::make_shared<ZSwcSpatialIndex>();
    idx->setZToXYRatio(zToXYRatio);
    idx->rebuild(*outSwc);
    tr.tw.traceMaskVolume = std::make_unique<ZSwcGeometryMaskVolume>(std::move(idx),
                                                                     signal.width(),
                                                                     signal.height(),
                                                                     signal.depth(),
                                                                     zToXYRatio,
                                                                     ZSwcGeometryMaskQuerySpace::ImageSpace);
  } else {
    auto swcMask = std::make_unique<ZSparseVoxelMask>(signal.width(), signal.height(), signal.depth());
    swcMask->clearU8(0);
    labelSwcIntoMaskLegacyLike(*outSwc, *swcMask, zToXYRatio, /*value*/ 255);
    tr.tw.traceMaskVolume = std::move(swcMask);
  }

  maybeCancel(tr.tw.cancellationToken);
  LocalNeuroseg seedLocseg;
  seedLocseg.seg.r1 = 3.0;
  seedLocseg.seg.c = 0.0;
  seedLocseg.seg.h = 11.0;
  seedLocseg.seg.theta = TzPiOver4LegacyLike;
  seedLocseg.seg.psi = 0.0;
  seedLocseg.seg.curvature = 0.0;
  seedLocseg.seg.alpha = 0.0;
  seedLocseg.seg.scale = 1.0;
  setNeurosegPositionLegacyLike(seedLocseg,
                                {position[0], position[1], position[2] * zToXYRatio},
                                NeuroposReferenceLegacyLike::Center);

  VLOG(1) << "Seed trace: optimize seed (attach) ...";
  (void)localNeurosegOptimizeWLegacyLike(seedLocseg, signal, zToXYRatio, /*option*/ 1, tr.tw.fitWorkspace);
  maybeCancel(tr.tw.cancellationToken);

  TraceRecord seedTr;
  traceRecordReset(seedTr);
  traceRecordSetFixPoint(seedTr, 0.0);
  traceRecordSetDirection(seedTr, TraceDirection::BothDir);

  LocsegChain chain;
  LocsegNode node;
  node.locseg = seedLocseg;
  node.tr = seedTr;
  (void)chain.addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

  traceWorkspaceSetTraceStatusLegacyLike(tr.tw, TraceStatus::Normal, TraceStatus::Normal);
  VLOG(1) << "Seed trace: tracing chain (attach) ...";
  traceLocsegLegacyLike(signal, zToXYRatio, chain, tr.tw);
  maybeCancel(tr.tw.cancellationToken);
  (void)locsegChainRemoveOverlapEndsLegacyLike(chain);
  locsegChainRemoveTurnEndsLegacyLike(chain, 1.0);

  tr.circles = locsegChainToGeo3dCircleArrayZLegacyLike(chain, zToXYRatio);
  if (tr.circles.empty()) {
    LOG(INFO) << "Seed trace (attach): no circles produced.";
    return {.swc = std::move(outSwc), .newNodes = 0};
  }

  const auto [start, end] = trimmedCircleIndexRangeLegacyLike(tr.tw, tr.circles);
  const int trimmedCount = end - start;
  if (start >= end || trimmedCount <= 1) {
    LOG(INFO) << "Seed trace (attach): circle range empty after trimming.";
    return {.swc = std::move(outSwc), .newNodes = 0};
  }

  ZSwc::SwcTreeNode parent = outSwc->end();
  ZSwc::SwcTreeNode branchRoot = outSwc->end();

  size_t added = 0;
  for (int i = start; i < end; ++i) {
    const Geo3dCircle& circle = tr.circles[static_cast<size_t>(i)];
    SwcNode swcNode(/*id*/ 1,
                    /*type*/ 0,
                    circle.center[0],
                    circle.center[1],
                    circle.center[2],
                    circle.radius,
                    /*parentID*/ -1);
    swcNode.selected = true;

    if (i == start) {
      parent = outSwc->appendRoot(swcNode);
      branchRoot = parent;
    } else {
      parent = outSwc->appendChild(parent, swcNode);
    }
    ++added;
  }

  if (!ZSwc::isNull(branchRoot)) {
    connectBranchToHostLegacyLike(*outSwc, hostRoots, branchRoot, signal);
  } else {
    LOG(ERROR) << "traceSeedIntoHostSwcLegacyLike: internal error (null branch root after append).";
  }

  // `ZNeuronConstructor::reconstruct` resorts IDs before returning its ZSwcTree.
  resortId(*outSwc);

  LOG(INFO) << fmt::format("Seed trace (attach): done (newNodes={})", added);
  return {.swc = std::move(outSwc), .newNodes = added};
}

} // namespace

SeedTraceResult traceSeedNewSwcLegacyLike(const ZImg& signal,
                                          const std::array<double, 3>& position,
                                          const TraceConfig& cfg,
                                          double zToXYRatio,
                                          size_t c,
                                          size_t t,
                                          folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    return {};
  }

  const ZImg signalView = traceSignalViewLegacyLike(signal, c, t);
  return traceSeedNewSwcFromSignalViewLegacyLike(signalView, position, cfg, zToXYRatio, cancellationToken);
}

SeedTraceResult traceSeedNewSwcLegacyLike(const ZVoxelVolume& signal,
                                          const std::array<double, 3>& position,
                                          const TraceConfig& cfg,
                                          double zToXYRatio,
                                          folly::CancellationToken cancellationToken)
{
  return traceSeedNewSwcFromSignalViewLegacyLike(signal, position, cfg, zToXYRatio, cancellationToken);
}

SeedTraceResult traceSeedIntoHostSwcLegacyLike(const ZImg& signal,
                                               const ZSwc& hostSwc,
                                               const std::array<double, 3>& position,
                                               const TraceConfig& cfg,
                                               double zToXYRatio,
                                               size_t c,
                                               size_t t,
                                               folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    auto outSwc = std::make_unique<ZSwc>(hostSwc);
    for (auto& tn : *outSwc) {
      tn.selected = false;
    }
    return {.swc = std::move(outSwc), .newNodes = 0};
  }

  const ZImg signalView = traceSignalViewLegacyLike(signal, c, t);
  return traceSeedIntoHostSwcFromSignalViewLegacyLike(signalView,
                                                      hostSwc,
                                                      position,
                                                      cfg,
                                                      zToXYRatio,
                                                      cancellationToken);
}

SeedTraceResult traceSeedIntoHostSwcLegacyLike(const ZVoxelVolume& signal,
                                               const ZSwc& hostSwc,
                                               const std::array<double, 3>& position,
                                               const TraceConfig& cfg,
                                               double zToXYRatio,
                                               folly::CancellationToken cancellationToken)
{
  return traceSeedIntoHostSwcFromSignalViewLegacyLike(signal, hostSwc, position, cfg, zToXYRatio, cancellationToken);
}

} // namespace nim
