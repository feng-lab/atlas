#include "zneutubetraceallseeds.h"

#include "zneutubedarrayqsort.h"
#include "zneutubelocsegchainmetrics.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubemathutils.h"
#include "zneutubetracelocseglabel.h"

#include "zcancellation.h"
#include "zlog.h"

#include <cmath>

namespace nim {

namespace {

[[nodiscard]] int maskValueAt(const ZImg& mask, int x, int y, int z)
{
  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());
  if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
    return 0;
  }

  return imgTypeDispatcher(mask.info(), [&]<typename TVoxel>() -> int {
    return static_cast<int>(*mask.data<TVoxel>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
  });
}

} // namespace

std::vector<std::unique_ptr<LocsegChain>> traceAllSeedsLegacyLike(const ZImg& signal,
                                                                  double zScale,
                                                                  std::vector<LocalNeuroseg>& locsegArray,
                                                                  std::vector<double>& scores,
                                                                  TraceWorkspace& tw)
{
  CHECK(scores.size() == locsegArray.size());

  const int nseed = static_cast<int>(locsegArray.size());
  if (nseed <= 0) {
    return {};
  }

  std::vector<int> indices;
  darrayQsortLegacy(scores, &indices);
  CHECK(static_cast<int>(indices.size()) == nseed);

  // Ensure trace mask exists if we're updating it (legacy allocates it lazily here).
  if (tw.traceMaskUpdating) {
    traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ false);
  }

  LocsegLabelWorkspaceLegacyLike labelWs;
  labelWs.signal = &signal;

  std::vector<std::unique_ptr<LocsegChain>> chains;
  chains.reserve(static_cast<size_t>(nseed));

  for (int i = nseed - 1; i >= 0; --i) {
    maybeCancel(tw.cancellationToken);

    const int seedIndex = indices[static_cast<size_t>(i)];
    CHECK(seedIndex >= 0 && seedIndex < nseed);

    const LocalNeuroseg& seedLocseg = locsegArray[static_cast<size_t>(seedIndex)];
    const double seedScore = scores[static_cast<size_t>(i)];

    if (!localNeurosegGoodScoreLegacyLike(seedLocseg, seedScore, tw.minScore)) {
      continue;
    }

    const double width = neurosegRBLegacyLike(seedLocseg.seg);
    if (width > tw.dyvar[0]) {
      continue;
    }

    traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::Normal, TraceStatus::Normal);

    if (tw.traceMask) {
      std::array<double, 3> pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h / 3.0);
      int tmpx = iroundLegacyLike(pt[0]);
      int tmpy = iroundLegacyLike(pt[1]);
      int tmpz = iroundLegacyLike(pt[2] * zScale);
      if (maskValueAt(*tw.traceMask, tmpx, tmpy, tmpz) > 0) {
        tw.traceStatus[0] = TraceStatus::HitMark;
      }

      pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h * 2.0 / 3.0);
      tmpx = iroundLegacyLike(pt[0]);
      tmpy = iroundLegacyLike(pt[1]);
      tmpz = iroundLegacyLike(pt[2] * zScale);
      if (maskValueAt(*tw.traceMask, tmpx, tmpy, tmpz) > 0) {
        tw.traceStatus[1] = TraceStatus::HitMark;
      }
    }

    if (tw.traceStatus[0] != TraceStatus::Normal && tw.traceStatus[1] != TraceStatus::Normal) {
      continue;
    }

    TraceRecord tr;
    traceRecordReset(tr);
    traceRecordSetDirection(tr, TraceDirection::BothDir);

    auto chain = std::make_unique<LocsegChain>();
    LocsegNode node;
    node.locseg = seedLocseg;
    node.tr = tr;
    (void)chain->addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

    traceLocsegLegacyLike(signal, zScale, *chain, tw);
    (void)locsegChainRemoveOverlapEndsLegacyLike(*chain);
    locsegChainRemoveTurnEndsLegacyLike(*chain, 1.0);

    const bool keep = (locsegChainGeolenLegacyLike(*chain) >= tw.minChainLength) ||
                      (tw.traceStatus[0] == TraceStatus::HitMark) || (tw.traceStatus[1] == TraceStatus::HitMark);
    if (!keep) {
      continue;
    }

    if (tw.traceMaskUpdating && tw.traceMask) {
      labelWs.sratio = 1.5;
      labelWs.sdiff = 0.0;
      labelWs.option = 1;
      // Atlas uses a binary "already traced" mask (0/1). The migrated algorithm only checks mask voxels as
      // a boolean (>0), so a constant value is sufficient and avoids uint8 overflow edge cases.
      labelWs.value = 1;
      labelWs.flag = 0;
      locsegChainLabelWLegacyLike(*chain,
                                  *tw.traceMask,
                                  zScale,
                                  /*begin*/ 0,
                                  /*end*/ chain->length() - 1,
                                  labelWs);
    }

    chains.push_back(std::move(chain));
  }

  return chains;
}

} // namespace nim
