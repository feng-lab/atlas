#include "zneutubetraceauto.h"

#include "zneutubeneuronstructure.h"
#include "zneutubeimgbinarizer.h"
#include "zneutubetracemask.h"
#include "zneutubetraceallseeds.h"
#include "zneutubetracechainscreen.h"
#include "zneutubetracerecover.h"
#include "zneutubetraceseed.h"
#include "zneutubetraceseeder.h"
#include "zneutubetracescorethresholds.h"
#include "zneutubetraceworkspace.h"
#include "zswcops.h"
#include "zswcpostprocess.h"
#include "zswcresampler.h"
#include "zcancellation.h"

#include "zlog.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace nim {

namespace {

void reorderRootListByZDescendingInPlaceLegacyLike(ZSwc& tree)
{
  if (tree.numRoots() <= 1) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> roots;
  roots.reserve(tree.numRoots());
  for (auto it = tree.beginRoot(); it != tree.endRoot(); ++it) {
    roots.emplace_back(it);
  }

  auto* head = roots.front().node->prevSibling;
  auto* tail = roots.back().node->nextSibling;

  // Match the legacy forest write order observed in NeuTu auto-trace outputs, where disconnected components
  // are emitted in descending z of their root node. This is an ordering-only normalization; it does not
  // change geometry/topology inside each component, but it makes depth-first ID resorting deterministic
  // for A/B parity.
  std::stable_sort(roots.begin(), roots.end(), [](const ZSwc::SwcTreeNode& a, const ZSwc::SwcTreeNode& b) {
    return a->z > b->z;
  });

  CHECK(head != nullptr);
  CHECK(tail != nullptr);

  head->nextSibling = roots.front().node;
  roots.front().node->prevSibling = head;

  for (size_t i = 1; i < roots.size(); ++i) {
    roots[i - 1].node->nextSibling = roots[i].node;
    roots[i].node->prevSibling = roots[i - 1].node;
  }

  roots.back().node->nextSibling = tail;
  tail->prevSibling = roots.back().node;
}

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

} // namespace

std::unique_ptr<ZSwc> traceNeuronAutoLegacyLike(ZImg signal,
                                                const TraceConfig& cfg,
                                                bool /*diagnosis*/,
                                                bool /*verbose*/,
                                                bool doResampleAfterTracing,
                                                const ZImg* predefinedMask,
                                                folly::CancellationToken cancellationToken)
{
  if (signal.isEmpty()) {
    return nullptr;
  }
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);

  AutoTraceContextLegacyLike ctx;
  ctx.signal = std::move(signal);
  ctx.cfg = &cfg;

  locsegChainDefaultTraceWorkspaceLegacyLike(ctx.tw, ctx.signal);
  ctx.tw.cancellationToken = std::move(cancellationToken);
  maybeCancel(ctx.tw.cancellationToken);

  ctx.tw.refit = cfg.refit;
  ctx.tw.tuneEnd = cfg.tuneEnd;
  ctx.tw.traceMaskUpdating = ctx.maskTracing;

  traceWorkspaceInitTraceMaskLegacyLike(ctx.tw, ctx.signal, /*clearing*/ false);
  maybeCancel(ctx.tw.cancellationToken);

  // Legacy default preprocess: subtract background and optionally invert bright-background images.
  // Bright-background handling is not yet ported (Atlas currently assumes dark background).
  (void)subtractBackgroundLegacyLike(ctx.signal, /*minFr*/ 0.5, /*maxIter*/ 3);
  maybeCancel(ctx.tw.cancellationToken);

  // Mask + seeds.
  MakeMaskDiagnosticsLegacyLike maskDiag;
  if (predefinedMask != nullptr) {
    ctx.mask = *predefinedMask;
  } else {
    ctx.mask = makeMaskLegacyLike(ctx.signal, cfg, &maskDiag);
  }
  if (!ctx.mask) {
    return nullptr;
  }
  maybeCancel(ctx.tw.cancellationToken);

  Geo3dScalarField seeds = extractSeedOriginalLegacyLike(*ctx.mask);

  RemoveNoisySeedDiagnosticsLegacyLike seedDiag;
  seeds = removeNoisySeedLegacyLike(std::move(seeds), *ctx.mask, cfg.seedMethod, ctx.screeningSeed, &seedDiag);
  seeds = removeTracedSeedLegacyLike(seeds, ctx.tw);

  prepareTraceScoreThresholdLegacyLike(ctx.signal, cfg, TracingModeLegacyLike::Seed, ctx.tw);

  SeedSortResultLegacyLike sorted = sortSeedsLegacyLike(seeds, ctx.signal, ctx.tw);
  ctx.baseMask = sorted.baseMask;

  prepareTraceScoreThresholdLegacyLike(ctx.signal, cfg, TracingModeLegacyLike::Auto, ctx.tw);
  std::vector<std::unique_ptr<LocsegChain>> chains =
    traceAllSeedsLegacyLike(ctx.signal, /*zScale*/ 1.0, sorted.locsegArray, sorted.scoreArray, ctx.tw);
  maybeCancel(ctx.tw.cancellationToken);

  if (cfg.recover > 0) {
    RecoverResultLegacyLike recovered = recoverLegacyLike(ctx.signal, cfg, *ctx.mask, std::move(ctx.baseMask), ctx.tw);
    ctx.baseMask = std::move(recovered.baseMask);
    for (auto& c : recovered.chains) {
      chains.push_back(std::move(c));
    }
  }
  maybeCancel(ctx.tw.cancellationToken);

  if (cfg.chainScreenCount > 0 && static_cast<int>(chains.size()) > cfg.chainScreenCount) {
    screenChainsLegacyLike(ctx.signal, chains);
  }
  maybeCancel(ctx.tw.cancellationToken);

  ConnectionTestWorkspaceLegacyLike ctw;
  defaultConnectionTestWorkspaceLegacyLike(ctw);
  ctw.spTest = cfg.spTest;
  ctw.crossoverTest = cfg.crossoverTest;
  ctw.distThre = cfg.maxEucDist;

  if (chains.size() > 500) {
    ctw.spTest = false;
  }

  CHECK(ctx.maskTracing) << "traceNeuronAutoLegacyLike: maskTracing=false path is not ported yet.";

  NeuronStructureChainsLegacyLike ns = locsegChainCompNeurostructLegacyLike(chains, &ctx.signal, /*zScale*/ 1.0, ctw);
  processNeuronStructureLegacyLike(ns);

  CHECK(!ctw.crossoverTest) << "traceNeuronAutoLegacyLike: crossoverTest is not ported yet.";

  NeuronStructureCirclesLegacyLike ns2 =
    neuronStructureLocsegChainToCircleSLegacyLike(ns, /*xyScale*/ 1.0, /*zScale*/ 1.0);
  neuronStructureToTreeLegacyLike(ns2);

  std::unique_ptr<ZSwc> tree = neuronStructureToSwcTreeCircleZLegacyLike(ns2, /*zScale*/ 1.0);
  if (!tree || tree->empty()) {
    return nullptr;
  }

  // `ZNeuronConstructor::reconstruct` resorts IDs before returning its ZSwcTree.
  reorderRootListByZDescendingInPlaceLegacyLike(*tree);
  resortId(*tree);

  swcTreeRemoveZigzagLegacyLike(*tree);
  swcTreeTuneBranchLegacyLike(*tree);
  swcTreeRemoveSpurLegacyLike(*tree);
  swcTreeMergeCloseNodeLegacyLike(*tree, /*threshold*/ 0.01);
  swcTreeRemoveOvershootLegacyLike(*tree);

  if (doResampleAfterTracing) {
    ZNeutubeSwcResampler resampler;
    resampler.optimalDownsample(*tree);
  }
  maybeCancel(ctx.tw.cancellationToken);

  // `ZNeuronTracer::trace` calls `ZSwcPruner::removeOrphanBlob` with minLength=0.
  swcTreeRemoveOrphanBlobLegacyLike(*tree, /*minLength*/ 0.0, /*minOrphanCount*/ 10);

  return tree;
}

} // namespace nim
