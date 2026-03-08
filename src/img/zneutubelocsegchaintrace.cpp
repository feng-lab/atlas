#include "zneutubelocsegchaintrace.h"

#include "zneutubelocsegchain.h"
#include "zneutubestackfitoptions.h"
#include "zneutubeneuroseg.h"

#include "zcancellation.h"
#include "zlog.h"
#include "zvoxelvolume.h"

#include <cmath>

namespace nim {

namespace {

[[nodiscard]] double dmax2LegacyLike(double a, double b)
{
  return (a > b) ? a : b;
}

void recordTraceScoreLegacyLike(TraceRecord& record, const LocsegFitWorkspace& fitWorkspace)
{
  // Port of `RECORD_TRACE_SCORE(record, fit_workspace)` macro.
  const int n = fitWorkspace.sws.fs.n;
  CHECK(n > 0);
  record.fs.scores[0] = fitWorkspace.sws.fs.scores[0];
  record.fs.scores[1] = fitWorkspace.sws.fs.scores[static_cast<size_t>(n - 1)];
}

void locsegChainBeginFitLegacyLike(LocalNeuroseg& locseg,
                                   LocsegChainEndLegacyLike listEnd,
                                   TraceDirection& traceDirection)
{
  // Port of `LOCSEG_CHAIN_BEGIN_FIT(locseg, list_end)` macro.
  if (listEnd == LocsegChainEndLegacyLike::Head) {
    traceDirection = TraceDirection::Backward;
    flipLocalNeurosegLegacyLike(locseg);
  } else {
    traceDirection = TraceDirection::Forward;
  }
}

void locsegChainEndFitLegacyLike(LocalNeuroseg& locseg, LocsegChainEndLegacyLike listEnd)
{
  // Port of `LOCSEG_CHAIN_END_FIT(locseg, list_end)` macro.
  if (listEnd == LocsegChainEndLegacyLike::Head) {
    flipLocalNeurosegLegacyLike(locseg);
  }
}

void locsegChainFitLegacyLike(LocalNeuroseg& locseg,
                              const ZImg& stack,
                              double zToXYRatio,
                              LocsegFitWorkspace& fitWs,
                              TraceRecord& tr)
{
  // Port of `LOCSEG_CHAIN_FIT(locseg)` macro.
  (void)fitLocalNeurosegWLegacyLike(locseg, stack, zToXYRatio, fitWs);
  recordTraceScoreLegacyLike(tr, fitWs);
}

template<typename StackLike>
void locsegChainFitLegacyLike(LocalNeuroseg& locseg,
                              const StackLike& stack,
                              double zToXYRatio,
                              LocsegFitWorkspace& fitWs,
                              TraceRecord& tr)
{
  // Port of `LOCSEG_CHAIN_FIT(locseg)` macro.
  (void)fitLocalNeurosegWLegacyLike(locseg, stack, zToXYRatio, fitWs);
  recordTraceScoreLegacyLike(tr, fitWs);
}

} // namespace

template<typename StackLike>
[[nodiscard]] TraceStatus locsegChainTraceTestLegacyLikeImpl(const LocalNeuroseg* locseg,
                                                             const LocsegChain* chain,
                                                             const TraceWorkspace& tw,
                                                             TraceRecord* tr,
                                                             double zToXYRatio,
                                                             double maxR,
                                                             TraceDirection traceDirection,
                                                             double minR,
                                                             const LocalNeuroseg* prevLocseg,
                                                             const StackLike& stack)
{
  std::array<double, 3> pos = {-1.0, -1.0, -1.0};
  double segr1 = 1.0;
  double segr2 = 1.0;

  if (locseg != nullptr) {
    if (traceDirection == TraceDirection::Forward) {
      pos = localNeurosegTopLegacyLike(*locseg);
    } else if (traceDirection == TraceDirection::Backward) {
      pos = localNeurosegBottomLegacyLike(*locseg);
    } else {
      pos[0] = -1.0;
    }

    if (tw.resolution[0] > 0.0) {
      segr1 = std::sqrt(neurosegRxPLegacyLike(locseg->seg, tw.resolution, NeuroposReferenceLegacyLike::Bottom) *
                        neurosegRyPLegacyLike(locseg->seg, tw.resolution, NeuroposReferenceLegacyLike::Bottom));
      segr2 = std::sqrt(neurosegRxPLegacyLike(locseg->seg, tw.resolution, NeuroposReferenceLegacyLike::Top) *
                        neurosegRyPLegacyLike(locseg->seg, tw.resolution, NeuroposReferenceLegacyLike::Top));
    } else {
      segr1 = std::sqrt(neurosegRxLegacyLike(locseg->seg, NeuroposReferenceLegacyLike::Bottom) *
                        neurosegRyLegacyLike(locseg->seg, NeuroposReferenceLegacyLike::Bottom));
      segr2 = std::sqrt(neurosegRxLegacyLike(locseg->seg, NeuroposReferenceLegacyLike::Top) *
                        neurosegRyLegacyLike(locseg->seg, NeuroposReferenceLegacyLike::Top));
    }
  }

  std::array<double, 3> ipos = pos;
  ipos[2] /= zToXYRatio;

  if (tr != nullptr && locseg != nullptr) {
    if (!localNeurosegGoodScoreLegacyLike(*locseg, tr->fs.scores[static_cast<size_t>(tw.tscoreOption)], tw.minScore)) {
      return TraceStatus::LowScore;
    }

    if (tw.supStack != nullptr) {
      StackFitScore fs{};
      fs.n = 1;
      fs.options[0] = tw.tscoreOption;
      const double supScore = localNeurosegScorePLegacyLike(*locseg, *tw.supStack, zToXYRatio, &fs);
      if (!localNeurosegGoodScoreLegacyLike(*locseg, supScore, tw.minScore)) {
        return TraceStatus::LowScore;
      }
    }
  }

  if (locseg != nullptr) {
    if (segr1 > maxR || segr2 > maxR) {
      return TraceStatus::TooLarge;
    }

    if (segr1 < minR || segr2 < minR) {
      return TraceStatus::TooSmall;
    }

    const double maxChange = maxR / 2.0;
    if (std::fabs(locseg->seg.c * locseg->seg.h) >= maxChange) {
      return TraceStatus::InvalidShape;
    }

    if (neurosegRALegacyLike(locseg->seg) > 2.5) {
      if (neurosegRBLegacyLike(locseg->seg) / neurosegRALegacyLike(locseg->seg) > 3.0) {
        return TraceStatus::InvalidShape;
      }
    }
  }

  if (chain != nullptr && pos[0] >= 0.0 && locseg != nullptr) {
    if ((locsegChainHitTestLegacyLike(*chain, TraceDirection::Backward, pos[0], pos[1], pos[2]) > 0) ||
        locsegChainFormLoopLegacyLike(*chain, *locseg, traceDirection)) {
      return TraceStatus::LoopFormed;
    }
  }

  if (prevLocseg != nullptr && locseg != nullptr) {
    const double r1 = neurosegCRCLegacyLike(prevLocseg->seg);
    const double r2 = neurosegCRCLegacyLike(locseg->seg);
    if (r2 > 1.0) {
      const double ratio = r1 / r2;
      if (ratio > 2.0 || ratio < 0.5) {
        return TraceStatus::SizeChange;
      }
    }

    const double intensity1 = localNeurosegAverageSignalLegacyLike(*locseg, stack, zToXYRatio);
    const double intensity2 = localNeurosegAverageSignalLegacyLike(*prevLocseg, stack, zToXYRatio);
    if (intensity1 / intensity2 < 0.5) {
      return TraceStatus::SignalChange;
    }
  }

  if (locseg != nullptr) {
    if (!traceWorkspacePointInBoundLegacyLike(tw, ipos)) {
      return TraceStatus::OutOfBound;
    }

    const int hitLabel = traceWorkspaceMaskValueLegacyLike(tw, ipos);
    if (hitLabel > 0) {
      if (tr != nullptr) {
        // Legacy writes directly without setting the record bitmask.
        tr->hitRegion = hitLabel;
      }
      return TraceStatus::HitMark;
    }
  }

  return TraceStatus::Normal;
}

TraceStatus locsegChainTraceTestLegacyLike(const LocalNeuroseg* locseg,
                                           const LocsegChain* chain,
                                           const TraceWorkspace& tw,
                                           TraceRecord* tr,
                                           double zToXYRatio,
                                           double maxR,
                                           TraceDirection traceDirection,
                                           double minR,
                                           const LocalNeuroseg* prevLocseg,
                                           const ZImg& stack)
{
  return locsegChainTraceTestLegacyLikeImpl(locseg,
                                            chain,
                                            tw,
                                            tr,
                                            zToXYRatio,
                                            maxR,
                                            traceDirection,
                                            minR,
                                            prevLocseg,
                                            stack);
}

TraceStatus locsegChainTraceTestLegacyLike(const LocalNeuroseg* locseg,
                                           const LocsegChain* chain,
                                           const TraceWorkspace& tw,
                                           TraceRecord* tr,
                                           double zToXYRatio,
                                           double maxR,
                                           TraceDirection traceDirection,
                                           double minR,
                                           const LocalNeuroseg* prevLocseg,
                                           const ZVoxelVolume& stack)
{
  return locsegChainTraceTestLegacyLikeImpl(locseg,
                                            chain,
                                            tw,
                                            tr,
                                            zToXYRatio,
                                            maxR,
                                            traceDirection,
                                            minR,
                                            prevLocseg,
                                            stack);
}

template<typename StackLike>
void traceLocsegLegacyLikeImpl(const StackLike& stack, double zToXYRatio, LocsegChain& chain, TraceWorkspace& tw)
{
  // Port of tz_locseg_chain.c::Trace_Locseg().
  if (chain.empty()) {
    return;
  }
  if (stack.isEmpty()) {
    return;
  }

  maybeCancel(tw.cancellationToken);

  LocsegFitWorkspace fitWs = tw.fitWorkspace;
  LocsegFitWorkspace fitWsH = tw.fitWorkspace;

  // Height-only variable set (NEUROSEG_VAR_MASK_HEIGHT).
  fitWsH.nvar = 1;
  fitWsH.varIndex[0] = 4;

  TraceRecord tr{};
  tr.mask = 0;
  tr.fs.n = 2;
  tr.fs.options[0] = 0;
  tr.fs.options[1] = tw.tscoreOption;

  // Append thresholding score option to the fit workspaces.
  CHECK(fitWs.sws.fs.n >= 0);
  CHECK(fitWs.sws.fs.n < static_cast<int>(fitWs.sws.fs.options.size()));
  fitWs.sws.fs.options[static_cast<size_t>(fitWs.sws.fs.n)] = tw.tscoreOption;
  ++fitWs.sws.fs.n;

  CHECK(fitWsH.sws.fs.n >= 0);
  CHECK(fitWsH.sws.fs.n < static_cast<int>(fitWsH.sws.fs.options.size()));
  fitWsH.sws.fs.options[static_cast<size_t>(fitWsH.sws.fs.n)] = tw.tscoreOption;
  ++fitWsH.sws.fs.n;

  TraceDirection traceDirection = TraceDirection::Unknown;

  // Score threshold for height fit.
  const double fitHeightThreshold = tw.dyvar[2];

  LocsegNode* currentEnds[2] = {chain.head(), chain.tail()};
  if (currentEnds[0] == nullptr || currentEnds[1] == nullptr) {
    return;
  }

  if (currentEnds[0] == currentEnds[1]) {
    traceRecordSetDirection(currentEnds[0]->tr, TraceDirection::BothDir);
  }

  int nrefit = 0;

  if (tw.fitFirst) {
    if (tw.traceStatus[1] == TraceStatus::Normal) {
      locsegChainFitLegacyLike(currentEnds[1]->locseg, stack, zToXYRatio, fitWs, tr);
      tw.traceStatus[1] = locsegChainTraceTestLegacyLike(&currentEnds[1]->locseg,
                                                         &chain,
                                                         tw,
                                                         &tr,
                                                         zToXYRatio,
                                                         tw.dyvar[0],
                                                         TraceDirection::Forward,
                                                         tw.dyvar[1],
                                                         nullptr,
                                                         stack);
    }

    if (tw.traceStatus[0] == TraceStatus::Normal) {
      flipLocalNeurosegLegacyLike(currentEnds[0]->locseg);
      locsegChainFitLegacyLike(currentEnds[0]->locseg, stack, zToXYRatio, fitWs, tr);
      flipLocalNeurosegLegacyLike(currentEnds[0]->locseg);

      tw.traceStatus[0] = locsegChainTraceTestLegacyLike(&currentEnds[0]->locseg,
                                                         &chain,
                                                         tw,
                                                         &tr,
                                                         zToXYRatio,
                                                         tw.dyvar[0],
                                                         TraceDirection::Backward,
                                                         tw.dyvar[1],
                                                         nullptr,
                                                         stack);
    }
  }

  StackFitScore ortFs{};
  ortFs.n = 1;
  ortFs.options[0] = static_cast<int>(StackFitOption::Corrcoef);

  const double defaultStep = tw.traceStep;
  double step[2] = {defaultStep, defaultStep};

  int hitCount[2] = {0, 0};

  const int chainLength = chain.length();
  if (chainLength > 1) {
    std::array<double, 3> pos{};
    if (tw.traceStatus[0] == TraceStatus::Normal || tw.traceStatus[0] == TraceStatus::Refit) {
      pos = localNeurosegBottomLegacyLike(currentEnds[0]->locseg);
      if (!traceWorkspacePointInBoundZLegacyLike(tw, pos, zToXYRatio)) {
        tw.traceStatus[0] = TraceStatus::OutOfBound;
      } else if (traceWorkspaceMaskValueZLegacyLike(tw, pos, zToXYRatio) > 0) {
        tw.traceStatus[0] = TraceStatus::HitMark;
      }
    }

    if (tw.traceStatus[1] == TraceStatus::Normal || tw.traceStatus[1] == TraceStatus::Refit) {
      pos = localNeurosegTopLegacyLike(currentEnds[1]->locseg);
      if (!traceWorkspacePointInBoundZLegacyLike(tw, pos, zToXYRatio)) {
        tw.traceStatus[1] = TraceStatus::OutOfBound;
      } else if (traceWorkspaceMaskValueZLegacyLike(tw, pos, zToXYRatio) > 0) {
        tw.traceStatus[1] = TraceStatus::HitMark;
      }
    }
  }

  TraceStatus curEndStatus = TraceStatus::Normal;
  int i = chainLength;

  auto tracingBackwardPossible = [&]() {
    return tw.traceStatus[0] == TraceStatus::Normal || tw.traceStatus[0] == TraceStatus::Refit;
  };

  auto tracingForwardPossible = [&]() {
    return tw.traceStatus[1] == TraceStatus::Normal || tw.traceStatus[1] == TraceStatus::Refit;
  };

  auto locsegChainRefit = [&](LocalNeuroseg& refitLocseg, LocsegChainEndLegacyLike listEnd, int endIndex) {
    maybeCancel(tw.cancellationToken);

    ++nrefit;
    LocsegNode* p = currentEnds[endIndex];
    CHECK(p != nullptr);

    CHECK(traceRecordFitHeight(p->tr, endIndex) != 1) << "Legacy invariant violated: fit_height already set";

    locsegChainBeginFitLegacyLike(p->locseg, listEnd, traceDirection);
    (void)localNeurosegHeightSearchWLegacyLike(p->locseg, stack, zToXYRatio, fitWs.sws);
    traceRecordSetFitHeight(p->tr, endIndex, 1);
    locsegChainEndFitLegacyLike(p->locseg, listEnd);

    curEndStatus = TraceStatus::Normal;
    while ((p->locseg.seg.h < NeurosegDefaultHLegacyLike / 3.0) && (i > 1)) {
      maybeCancel(tw.cancellationToken);

      chain.removeEnd(listEnd);
      --i;

      currentEnds[endIndex] = (listEnd == LocsegChainEndLegacyLike::Head) ? chain.head() : chain.tail();
      p = currentEnds[endIndex];
      CHECK(p != nullptr);

      if (dmax2LegacyLike(neurosegRxLegacyLike(p->locseg.seg, NeuroposReferenceLegacyLike::Center),
                          neurosegRyLegacyLike(p->locseg.seg, NeuroposReferenceLegacyLike::Center)) >=
          NeurosegDefaultHLegacyLike / 2.0) {
        curEndStatus = TraceStatus::TooLarge;
      } else {
        if (traceRecordFitHeight(p->tr, endIndex) == 0) {
          locsegChainBeginFitLegacyLike(p->locseg, listEnd, traceDirection);
          (void)localNeurosegHeightSearchWLegacyLike(p->locseg, stack, zToXYRatio, fitWs.sws);
          traceRecordSetFitHeight(p->tr, endIndex, 1);
          locsegChainEndFitLegacyLike(p->locseg, listEnd);
        } else {
          curEndStatus = TraceStatus::Repeated;
        }
      }
    }

    if (dmax2LegacyLike(neurosegRxLegacyLike(p->locseg.seg, NeuroposReferenceLegacyLike::Center),
                        neurosegRyLegacyLike(p->locseg.seg, NeuroposReferenceLegacyLike::Center)) >=
        NeurosegDefaultHLegacyLike / 2.0) {
      curEndStatus = TraceStatus::TooLarge;
    }

    locsegChainBeginFitLegacyLike(p->locseg, listEnd, traceDirection);
    if (tw.traceMaskVolume) {
      if (localNeurosegTopSampleLegacyLike(p->locseg, *tw.traceMaskVolume, zToXYRatio) > 0.0) {
        curEndStatus = TraceStatus::NotAssigned;
      }
    } else if (tw.traceMask) {
      if (localNeurosegTopSampleLegacyLike(p->locseg, *tw.traceMask, zToXYRatio) > 0.0) {
        curEndStatus = TraceStatus::NotAssigned;
      }
    }
    locsegChainEndFitLegacyLike(p->locseg, listEnd);

    if (curEndStatus == TraceStatus::Normal) {
      std::array<double, 3> boundPos{};
      double boundOffset = -p->locseg.seg.r1;
      if (listEnd == LocsegChainEndLegacyLike::Tail) {
        boundOffset = p->locseg.seg.h + neurosegR2LegacyLike(p->locseg.seg);
      }

      boundPos = localNeurosegAxisPositionLegacyLike(p->locseg, boundOffset);

      if ((boundPos[0] >= 0.0) && (boundPos[1] >= 0.0) && (boundPos[2] >= 0.0) &&
          (boundPos[0] < static_cast<double>(stack.width())) && (boundPos[1] < static_cast<double>(stack.height())) &&
          (boundPos[2] / zToXYRatio < static_cast<double>(stack.depth()))) {
        double posStep = 1.0 - neurosegR2LegacyLike(p->locseg.seg) / p->locseg.seg.h;
        if (posStep < 0.75) {
          posStep = 0.75;
        }

        locsegChainBeginFitLegacyLike(p->locseg, listEnd, traceDirection);
        nextLocalNeurosegLegacyLike(p->locseg, refitLocseg, posStep);
        locsegChainEndFitLegacyLike(p->locseg, listEnd);

        (void)localNeurosegOrientationSearchBLegacyLike(refitLocseg, stack, zToXYRatio, ortFs);

        if (NeuroposReference == NeuroposReferenceLegacyLike::Center) {
          localNeurosegPositionAdjustLegacyLike(refitLocseg, stack, zToXYRatio);
        }

        locsegChainFitLegacyLike(refitLocseg, stack, zToXYRatio, fitWs, tr);

        if (!localNeurosegGoodScoreLegacyLike(refitLocseg, tr.fs.scores[1], tw.minScore)) {
          (void)localNeurosegHeightSearchWLegacyLike(p->locseg, stack, zToXYRatio, fitWs.sws);
          locsegChainFitLegacyLike(refitLocseg, stack, zToXYRatio, fitWs, tr);
        }

        locsegChainEndFitLegacyLike(refitLocseg, listEnd);

        curEndStatus = locsegChainTraceTestLegacyLike(&refitLocseg,
                                                      &chain,
                                                      tw,
                                                      &tr,
                                                      zToXYRatio,
                                                      tw.dyvar[0],
                                                      traceDirection,
                                                      tw.dyvar[1],
                                                      &p->locseg,
                                                      stack);
      } else {
        curEndStatus = TraceStatus::SeedOutOfBound;
      }
    }
  };

  auto traceOneDirection = [&](LocsegChainEndLegacyLike listEnd, int endIndex, double traceStep) {
    maybeCancel(tw.cancellationToken);

    LocsegNode* endNode = currentEnds[endIndex];
    if (endNode == nullptr) {
      return;
    }

    curEndStatus = tw.traceStatus[endIndex];
    std::optional<LocalNeuroseg> currentLocseg;

    if (curEndStatus == TraceStatus::Normal) {
      LocalNeuroseg* prevLocseg = &endNode->locseg;

      locsegChainBeginFitLegacyLike(*prevLocseg, listEnd, traceDirection);

      currentLocseg = nextLocalNeurosegLegacyLike(*prevLocseg, traceStep);

      if (NeuroposReference == NeuroposReferenceLegacyLike::Center) {
        localNeurosegPositionAdjustLegacyLike(*currentLocseg, stack, zToXYRatio);
      }

      locsegChainFitLegacyLike(*currentLocseg, stack, zToXYRatio, fitWs, tr);

      if (tr.fs.scores[1] < fitHeightThreshold) {
        nextLocalNeurosegLegacyLike(*prevLocseg, *currentLocseg, traceStep);
        locsegChainFitLegacyLike(*currentLocseg, stack, zToXYRatio, fitWsH, tr);
        locsegChainFitLegacyLike(*currentLocseg, stack, zToXYRatio, fitWs, tr);
        prevLocseg->seg.h *= traceStep;
      }

      locsegChainEndFitLegacyLike(*prevLocseg, listEnd);
      locsegChainEndFitLegacyLike(*currentLocseg, listEnd);

      curEndStatus = locsegChainTraceTestLegacyLike(&(*currentLocseg),
                                                    &chain,
                                                    tw,
                                                    &tr,
                                                    zToXYRatio,
                                                    tw.dyvar[0],
                                                    traceDirection,
                                                    tw.dyvar[1],
                                                    prevLocseg,
                                                    stack);
    }

    if ((curEndStatus != TraceStatus::Normal) && tw.refit && (curEndStatus != TraceStatus::HitMark) &&
        (curEndStatus != TraceStatus::OutOfBound) && (curEndStatus != TraceStatus::NotAssigned) && (i > 1)) {
      curEndStatus = TraceStatus::Refit;
    }

    if (curEndStatus == TraceStatus::Refit) {
      CHECK(currentLocseg.has_value());
      locsegChainRefit(*currentLocseg, listEnd, endIndex);

      traceRecordSetRefit(tr, 1);

      if (nrefit > i + 1) {
        curEndStatus = TraceStatus::OverRefit;
      }
    }

    LocsegNode* currentNode = nullptr;
    switch (curEndStatus) {
      case TraceStatus::Normal: {
        CHECK(currentLocseg.has_value());
        LocsegNode node;
        node.locseg = *currentLocseg;
        node.tr = tr;
        currentNode = chain.addNode(std::move(node), listEnd);
        ++i;
        currentEnds[endIndex] = currentNode;
        hitCount[endIndex] = 0;
        break;
      }

      case TraceStatus::HitMark: {
        CHECK(currentLocseg.has_value());
        if (hitCount[endIndex] == -1) {
          LocsegNode node;
          node.locseg = *currentLocseg;
          node.tr = tr;
          currentNode = chain.addNode(std::move(node), listEnd);
          ++i;
          currentEnds[endIndex] = currentNode;
          ++hitCount[endIndex];
        } else {
          if (hitCount[endIndex] <= 0 && tw.addHit) {
            LocsegNode node;
            node.locseg = *currentLocseg;
            node.tr = tr;
            currentNode = chain.addNode(std::move(node), listEnd);
            ++i;
            currentEnds[endIndex] = currentNode;
          }

          tw.traceStatus[endIndex] = curEndStatus;
        }
        break;
      }

      case TraceStatus::OutOfBound: {
        CHECK(currentLocseg.has_value());
        LocsegNode node;
        node.locseg = *currentLocseg;
        node.tr = tr;
        currentNode = chain.addNode(std::move(node), listEnd);
        ++i;
        currentEnds[endIndex] = currentNode;
        tw.traceStatus[endIndex] = curEndStatus;
        break;
      }

      default:
        tw.traceStatus[endIndex] = curEndStatus;
        break;
    }
  };

  while ((i < tw.length) && (tracingBackwardPossible() || tracingForwardPossible())) {
    maybeCancel(tw.cancellationToken);

    if (tracingBackwardPossible()) {
      traceRecordReset(tr);
      traceRecordSetDirection(tr, TraceDirection::Backward);
      traceRecordSetFixPoint(tr, 1.0);
      traceOneDirection(LocsegChainEndLegacyLike::Head, 0, step[0]);
    }

    if (tracingForwardPossible()) {
      traceRecordReset(tr);
      traceRecordSetDirection(tr, TraceDirection::Forward);
      traceRecordSetFixPoint(tr, 0.0);
      traceOneDirection(LocsegChainEndLegacyLike::Tail, 1, step[1]);
    }
  }

  if (tw.tuneEnd) {
    if (chain.length() >= 2) {
      if ((tw.traceStatus[0] != TraceStatus::HitMark) && (tw.traceStatus[0] != TraceStatus::NotAssigned)) {
        auto* locseg = chain.headSeg();
        CHECK(locseg != nullptr);
        flipLocalNeurosegLegacyLike(*locseg);
        (void)localNeurosegHeightSearchWLegacyLike(*locseg, stack, zToXYRatio, fitWs.sws);
        flipLocalNeurosegLegacyLike(*locseg);
      }

      if ((tw.traceStatus[1] != TraceStatus::HitMark) && (tw.traceStatus[1] != TraceStatus::NotAssigned)) {
        auto* locseg = chain.tailSeg();
        CHECK(locseg != nullptr);
        (void)localNeurosegHeightSearchWLegacyLike(*locseg, stack, zToXYRatio, fitWs.sws);
      }

      (void)locsegChainRemoveOverlapEndsLegacyLike(chain);
    }
  }

  --fitWs.sws.fs.n;
  --fitWsH.sws.fs.n;
}

void traceLocsegLegacyLike(const ZImg& stack, double zToXYRatio, LocsegChain& chain, TraceWorkspace& tw)
{
  traceLocsegLegacyLikeImpl(stack, zToXYRatio, chain, tw);
}

void traceLocsegLegacyLike(const ZVoxelVolume& stack, double zToXYRatio, LocsegChain& chain, TraceWorkspace& tw)
{
  traceLocsegLegacyLikeImpl(stack, zToXYRatio, chain, tw);
}

} // namespace nim
