#include "zneutubetraceauto.h"

#include "zneutubeneuronstructure.h"
#include "zneutubeimgbinarizer.h"
#include "zneutubetracemask.h"
#include "zneutubetraceallseeds.h"
#include "zneutubetracechainscreen.h"
#include "zneutubetracerecover.h"
#include "zneutubetraceseed.h"
#include "zneutubetraceseeder.h"
#include "zneutubetracelocseglabel.h"
#include "zneutubetracescorethresholds.h"
#include "zneutubetraceworkspace.h"
#include "zneutubetracezscale.h"
#include "zswcops.h"
#include "zswcpostprocess.h"
#include "zswcresampler.h"
#include "zcancellation.h"

#include "zlog.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

DECLARE_bool(atlas_trace_use_swc_geometry_mask);
DEFINE_bool(atlas_autotrace_use_swc_geometry_mask,
            false,
            "In-memory auto tracing: allow using the SWC-geometry trace mask (spatial index) when available. "
            "This is gated by --atlas_trace_use_swc_geometry_mask (shared across tracing modes) but defaults to "
            "false because it is currently slower in whole-volume auto trace. "
            "Interactive tracing remains controlled by --atlas_trace_use_swc_geometry_mask.");
DECLARE_double(atlas_trace_mask_exclusion_swell_ratio);
DECLARE_double(atlas_trace_mask_exclusion_swell_diff);
DECLARE_double(atlas_trace_mask_exclusion_swell_limit);

namespace nim {

namespace {

struct AutoTraceContextLegacyLike
{
  ZImg signal;
  const TraceConfig* cfg = nullptr;

  TraceWorkspace tw;

  // Working masks (legacy members: m_mask and m_baseMask).
  std::optional<ZImg> mask;
  std::optional<ZImg> baseMask;

  bool screeningSeed = true;
  bool maskTracing = true;
};

void materializeTraceMaskFromChainsLegacyLike(const ZImg& signal,
                                              const std::vector<std::unique_ptr<LocsegChain>>& chains,
                                              double zToXYRatio,
                                              TraceWorkspace& tw)
{
  traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ true);
  CHECK(tw.traceMask != nullptr);

  LocsegLabelWorkspaceLegacyLike labelWs;
  labelWs.signal = &signal;
  labelWs.sratio = FLAGS_atlas_trace_mask_exclusion_swell_ratio;
  labelWs.sdiff = FLAGS_atlas_trace_mask_exclusion_swell_diff;
  labelWs.slimit = FLAGS_atlas_trace_mask_exclusion_swell_limit;
  labelWs.option = 1;
  labelWs.value = 1;
  labelWs.flag = 0;

  for (const auto& chain : chains) {
    if (!chain || chain->empty()) {
      continue;
    }

    locsegChainLabelWLegacyLike(*chain,
                                *tw.traceMask,
                                zToXYRatio,
                                /*begin*/ 0,
                                /*end*/ chain->length() - 1,
                                labelWs);
  }
}

} // namespace

std::unique_ptr<ZSwc> traceNeuronAutoLegacyLike(ZImg signal,
                                                const TraceConfig& cfg,
                                                double zToXYRatio,
                                                bool /*diagnosis*/,
                                                bool /*verbose*/,
                                                bool doResampleAfterTracing,
                                                const ZImg* predefinedMask,
                                                folly::CancellationToken cancellationToken)
{
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  if (signal.isEmpty()) {
    return nullptr;
  }
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);

  LOG(INFO) << fmt::format("Auto trace: start (size={}x{}x{}, type={}, resample={})",
                           signal.width(),
                           signal.height(),
                           signal.depth(),
                           signal.info(),
                           doResampleAfterTracing);
  VLOG(1) << fmt::format("Auto trace: zToXYRatio={:.6g}", zToXYRatio);

  AutoTraceContextLegacyLike ctx;
  ctx.signal = std::move(signal);
  ctx.cfg = &cfg;

  locsegChainDefaultTraceWorkspaceLegacyLike(ctx.tw, ctx.signal);
  ctx.tw.cancellationToken = std::move(cancellationToken);
  maybeCancel(ctx.tw.cancellationToken);

  ctx.tw.refit = cfg.refit;
  ctx.tw.tuneEnd = cfg.tuneEnd;
  ctx.tw.traceMaskUpdating = ctx.maskTracing;
  // Keep the legacy auto-trace first pass behavior here: `TraceWorkspace::resolution` is an opt-in branch
  // in the locseg trace test, not inert metadata, and the historical auto path leaves it unset.

  // Legacy default preprocess: subtract background and optionally invert bright-background images.
  // Bright-background handling is not yet ported (Atlas currently assumes dark background).
  LOG(INFO) << "Auto trace: preprocess (subtract background) ...";
  const int commonIntensity = subtractBackgroundLegacyLike(ctx.signal, /*minFr*/ 0.5, /*maxIter*/ 3);
  VLOG(1) << fmt::format("Auto trace: subtract background done (commonIntensity={})", commonIntensity);
  maybeCancel(ctx.tw.cancellationToken);

  // Mask + seeds.
  MakeMaskDiagnosticsLegacyLike maskDiag;
  if (predefinedMask != nullptr) {
    LOG(INFO) << "Auto trace: using predefined mask.";
    ctx.mask = *predefinedMask;
  } else {
    LOG(INFO) << "Auto trace: make mask ...";
    ctx.mask = makeMaskLegacyLike(ctx.signal, cfg, &maskDiag);
  }
  if (!ctx.mask) {
    LOG(INFO) << "Auto trace: make mask failed (null mask).";
    return nullptr;
  }
  maybeCancel(ctx.tw.cancellationToken);
  VLOG(1) << fmt::format("Auto trace: mask threshold={}", maskDiag.binarizeThreshold);

  LOG(INFO) << "Auto trace: extract seeds ...";
  Geo3dScalarField seeds = extractSeedOriginalLegacyLike(*ctx.mask, ctx.tw.cancellationToken);
  LOG(INFO) << fmt::format("Auto trace: extracted {} seeds.", seeds.size());

  RemoveNoisySeedDiagnosticsLegacyLike seedDiag;
  LOG(INFO) << "Auto trace: remove noisy seeds ...";
  seeds = removeNoisySeedLegacyLike(std::move(seeds),
                                    *ctx.mask,
                                    cfg.seedMethod,
                                    ctx.screeningSeed,
                                    ctx.tw.cancellationToken,
                                    &seedDiag);
  LOG(INFO) << fmt::format("Auto trace: {} seeds after noise removal.", seeds.size());
  seeds = removeTracedSeedLegacyLike(seeds, ctx.tw);
  VLOG(1) << fmt::format("Auto trace: {} seeds after removing traced hits.", seeds.size());

  LOG(INFO) << "Auto trace: prepare seed thresholds ...";
  prepareTraceScoreThresholdLegacyLike(ctx.signal, cfg, TracingModeLegacyLike::Seed, ctx.tw);

  LOG(INFO) << "Auto trace: score/sort seeds ...";
  SeedSortResultLegacyLike sorted = sortSeedsLegacyLike(seeds, ctx.signal, zToXYRatio, ctx.tw);
  ctx.baseMask = sorted.baseMask;

  LOG(INFO) << "Auto trace: prepare auto thresholds ...";
  prepareTraceScoreThresholdLegacyLike(ctx.signal, cfg, TracingModeLegacyLike::Auto, ctx.tw);

  LOG(INFO) << "Auto trace: trace all seeds ...";
  std::vector<std::unique_ptr<LocsegChain>> chains =
    traceAllSeedsLegacyLike(ctx.signal, zToXYRatio, sorted.locsegArray, sorted.scoreArray, ctx.tw);
  maybeCancel(ctx.tw.cancellationToken);
  LOG(INFO) << fmt::format("Auto trace: trace all seeds done (chains={}).", chains.size());

  if (cfg.recover > 0) {
    const bool useGeometryTraceMask =
      FLAGS_atlas_trace_use_swc_geometry_mask && FLAGS_atlas_autotrace_use_swc_geometry_mask;
    if (useGeometryTraceMask && !ctx.tw.traceMask && ctx.tw.traceMaskVolume) {
      LOG(INFO) << "Auto trace: materialize dense trace mask for legacy recovery ...";
      materializeTraceMaskFromChainsLegacyLike(ctx.signal, chains, zToXYRatio, ctx.tw);
    }
    LOG(INFO) << "Auto trace: recover ...";
    RecoverResultLegacyLike recovered =
      recoverLegacyLike(ctx.signal, cfg, zToXYRatio, *ctx.mask, std::move(ctx.baseMask), ctx.tw);
    ctx.baseMask = std::move(recovered.baseMask);
    for (auto& c : recovered.chains) {
      chains.push_back(std::move(c));
    }
    LOG(INFO) << fmt::format("Auto trace: recover done (total chains={}).", chains.size());
  }
  maybeCancel(ctx.tw.cancellationToken);

  if (cfg.chainScreenCount > 0 && static_cast<int>(chains.size()) > cfg.chainScreenCount) {
    LOG(INFO) << fmt::format("Auto trace: screen chains (count={}, limit={}) ...", chains.size(), cfg.chainScreenCount);
    screenChainsLegacyLike(ctx.signal, chains);
    LOG(INFO) << fmt::format("Auto trace: screen chains done (count={}).", chains.size());
  }
  maybeCancel(ctx.tw.cancellationToken);

  LOG(INFO) << "Auto trace: build neuron structure ...";
  ConnectionTestWorkspaceLegacyLike ctw;
  defaultConnectionTestWorkspaceLegacyLike(ctw);
  ctw.spTest = cfg.spTest;
  ctw.crossoverTest = cfg.crossoverTest;
  ctw.distThre = cfg.maxEucDist;
  // Reconstruction runs on chains that are already stored in isotropized tracing
  // space. Match the zrunneutucommand pipeline and keep chain-connection tests in
  // that space instead of introducing another anisotropic comparison transform.
  ctw.resolution = {1.0, 1.0, 1.0};

  if (chains.size() > 500) {
    ctw.spTest = false;
  }

  CHECK(ctx.maskTracing) << "traceNeuronAutoLegacyLike: maskTracing=false path is not ported yet.";

  NeuronStructureChainsLegacyLike ns =
    locsegChainCompNeurostructLegacyLike(chains, &ctx.signal, /*zToXYRatio*/ 1.0, ctw);
  processNeuronStructureLegacyLike(ns);

  CHECK(!ctw.crossoverTest) << "traceNeuronAutoLegacyLike: crossoverTest is not ported yet.";

  // `ns` still stores traced chains in isotropized tracing space here.
  // Keep circles/tree construction in that same space; the final SWC export below converts Z back to image space
  // with the real `zToXYRatio`.
  NeuronStructureCirclesLegacyLike ns2 =
    neuronStructureLocsegChainToCircleSLegacyLike(ns, /*xyScale*/ 1.0, /*zToXYRatio*/ 1.0);
  neuronStructureToTreeLegacyLike(ns2);

  std::unique_ptr<ZSwc> tree = neuronStructureToSwcTreeCircleZLegacyLike(ns2, zToXYRatio);
  if (!tree || tree->empty()) {
    LOG(INFO) << "Auto trace: no SWC output produced.";
    return nullptr;
  }

  // `ZNeuronConstructor::reconstruct` resorts IDs before returning its ZSwcTree.
  resortId(*tree);

  LOG(INFO) << fmt::format("Auto trace: SWC reconstructed (nodes={}).", tree->size());

  LOG(INFO) << "Auto trace: SWC postprocess (zigzag/tune/spur/merge/overshoot) ...";
  swcTreeRemoveZigzagLegacyLike(*tree);
  swcTreeTuneBranchLegacyLike(*tree);
  swcTreeRemoveSpurLegacyLike(*tree);
  swcTreeMergeCloseNodeLegacyLike(*tree, /*threshold*/ 0.01);
  swcTreeRemoveOvershootLegacyLike(*tree);

  if (doResampleAfterTracing) {
    LOG(INFO) << "Auto trace: resample (optimal) ...";
    ZNeutubeSwcResampler resampler;
    resampler.optimalDownsample(*tree);
  }
  maybeCancel(ctx.tw.cancellationToken);

  // `ZNeuronTracer::trace` calls `ZSwcPruner::removeOrphanBlob` with minLength=0.
  LOG(INFO) << "Auto trace: remove orphan blobs ...";
  swcTreeRemoveOrphanBlobLegacyLike(*tree, /*minLength*/ 0.0, /*minOrphanCount*/ 10);

  LOG(INFO) << fmt::format("Auto trace: done (nodes={}).", tree->size());
  return tree;
}

} // namespace nim
