#include "zneutubetraceinteractive.h"

#include "zneutubelocsegchain.h"
#include "zneutubelocsegchaincircle.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubetraceconnect.h"
#include "zneutubetraceworkspace.h"
#include "zneutubetraceswclabelstack.h"

#include "zsparsevoxelmask.h"
#include "zvoxelvolumedense.h"

#include "zcancellation.h"
#include "zlog.h"
#include "zswcops.h"

#include <array>
#include <utility>
#include <vector>

namespace nim {

namespace {

constexpr double TzPiOver4LegacyLike = 0.78539816339744830961566084582;

struct TraceCirclesResult
{
  std::vector<Geo3dCircle> circles;
  TraceWorkspace tw;
};

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

[[nodiscard]] TraceCirclesResult traceSeedToCirclesLegacyLike(const ZVoxelVolume& signal,
                                                              const std::array<double, 3>& position,
                                                              const TraceConfig& cfg,
                                                              const folly::CancellationToken& cancellationToken)
{
  TraceCirclesResult res;

  locsegChainDefaultTraceWorkspaceLegacyLike(res.tw, signal);
  res.tw.cancellationToken = cancellationToken;
  res.tw.refit = cfg.refit;
  res.tw.tuneEnd = cfg.tuneEnd;

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
  setNeurosegPositionLegacyLike(seedLocseg, position, NeuroposReferenceLegacyLike::Center);

  maybeCancel(res.tw.cancellationToken);
  (void)localNeurosegOptimizeWLegacyLike(seedLocseg, signal, 1.0, 1, res.tw.fitWorkspace);
  maybeCancel(res.tw.cancellationToken);

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
  traceLocsegLegacyLike(signal, 1.0, chain, res.tw);
  maybeCancel(res.tw.cancellationToken);
  (void)locsegChainRemoveOverlapEndsLegacyLike(chain);
  locsegChainRemoveTurnEndsLegacyLike(chain, 1.0);

  res.circles = locsegChainToGeo3dCircleArrayLegacyLike(chain);
  return res;
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

} // namespace

SeedTraceResult traceSeedNewSwcLegacyLike(const ZImg& signal,
                                          const std::array<double, 3>& position,
                                          const TraceConfig& cfg,
                                          size_t c,
                                          size_t t,
                                          folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    return {};
  }

  const ZImg signalView = traceSignalViewLegacyLike(signal, c, t);
  const ZDenseVoxelVolume vol(signalView);
  return traceSeedNewSwcLegacyLike(vol, position, cfg, cancellationToken);
}

SeedTraceResult traceSeedNewSwcLegacyLike(const ZVoxelVolume& signal,
                                          const std::array<double, 3>& position,
                                          const TraceConfig& cfg,
                                          folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    return {};
  }

  TraceCirclesResult tr = traceSeedToCirclesLegacyLike(signal, position, cfg, cancellationToken);
  if (tr.circles.empty()) {
    return {};
  }

  maybeCancel(cancellationToken);
  const auto [start, end] = trimmedCircleIndexRangeLegacyLike(tr.tw, tr.circles);
  if (start >= end) {
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
  return res;
}

SeedTraceResult traceSeedIntoHostSwcLegacyLike(const ZImg& signal,
                                               const ZSwc& hostSwc,
                                               const std::array<double, 3>& position,
                                               const TraceConfig& cfg,
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
  const ZDenseVoxelVolume vol(signalView);
  return traceSeedIntoHostSwcLegacyLike(vol, hostSwc, position, cfg, cancellationToken);
}

SeedTraceResult traceSeedIntoHostSwcLegacyLike(const ZVoxelVolume& signal,
                                               const ZSwc& hostSwc,
                                               const std::array<double, 3>& position,
                                               const TraceConfig& cfg,
                                               folly::CancellationToken cancellationToken)
{
  auto outSwc = std::make_unique<ZSwc>(hostSwc);
  for (auto& tn : *outSwc) {
    tn.selected = false;
  }

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

  if (signal.depth() == 1) {
    tr.tw.minScore = cfg.min2dScore;
  } else {
    tr.tw.minScore = cfg.minManualScore;
  }

  auto swcMask = std::make_unique<ZSparseVoxelMask>(signal.width(), signal.height(), signal.depth());
  swcMask->clearU8(0);
  labelSwcIntoMaskLegacyLike(*outSwc, *swcMask, /*zScale*/ 1.0, /*value*/ 255);
  tr.tw.traceMaskVolume = std::move(swcMask);

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
  setNeurosegPositionLegacyLike(seedLocseg, position, NeuroposReferenceLegacyLike::Center);

  (void)localNeurosegOptimizeWLegacyLike(seedLocseg, signal, 1.0, 1, tr.tw.fitWorkspace);
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
  traceLocsegLegacyLike(signal, 1.0, chain, tr.tw);
  maybeCancel(tr.tw.cancellationToken);
  (void)locsegChainRemoveOverlapEndsLegacyLike(chain);
  locsegChainRemoveTurnEndsLegacyLike(chain, 1.0);

  tr.circles = locsegChainToGeo3dCircleArrayLegacyLike(chain);
  if (tr.circles.empty()) {
    return {.swc = std::move(outSwc), .newNodes = 0};
  }

  const auto [start, end] = trimmedCircleIndexRangeLegacyLike(tr.tw, tr.circles);
  const int trimmedCount = end - start;
  if (start >= end || trimmedCount <= 1) {
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

  return {.swc = std::move(outSwc), .newNodes = added};
}

} // namespace nim
