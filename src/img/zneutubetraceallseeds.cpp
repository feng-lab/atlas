#include "zneutubetraceallseeds.h"

#include "zneutubedarrayqsort.h"
#include "zneutubelocsegchaincircle.h"
#include "zneutubelocsegchainmetrics.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubemathutils.h"
#include "zneutubetracelocseglabel.h"
#include "zneutubetracezscale.h"
#include "zswcgeometrymaskvolume.h"
#include "zswcspatialindex.h"

#include "zcancellation.h"
#include "zlog.h"

#include <gflags/gflags.h>

#include <cmath>

DECLARE_bool(atlas_trace_use_swc_geometry_mask);

namespace nim {

namespace {

constexpr double TraceMaskExclusionSwellRatioLegacyLike = 1.5;
constexpr double TraceMaskExclusionSwellDiffLegacyLike = 0.0;
constexpr double TraceMaskExclusionSwellLimitLegacyLike = 3.0;

[[nodiscard]] std::shared_ptr<ZSwcSpatialIndex>
ensureSpatialTraceMaskVolumeLegacyLike(TraceWorkspace& tw, const ZImg& signal, double zScale)
{
  CHECK(std::isfinite(zScale));
  CHECK(zScale > 0.0);

  std::shared_ptr<ZSwcSpatialIndex> index;
  if (tw.traceMaskVolume) {
    auto* spatialMask = dynamic_cast<ZSwcGeometryMaskVolume*>(tw.traceMaskVolume.get());
    CHECK(spatialMask != nullptr)
      << "traceAllSeedsLegacyLike: spatial trace-mask mode requires ZSwcGeometryMaskVolume.";
    index = spatialMask->sharedIndex();
    CHECK(index != nullptr);
    index->setZScale(zScale);
  } else {
    index = std::make_shared<ZSwcSpatialIndex>();
    index->setZScale(zScale);
  }

  tw.traceMaskVolume =
    std::make_unique<ZSwcGeometryMaskVolume>(index, signal.width(), signal.height(), signal.depth(), zScale);
  return index;
}

void insertChainIntoSpatialIndexLegacyLike(const LocsegChain& chain, ZSwcSpatialIndex& index)
{
  const std::vector<Geo3dCircle> circles =
    locsegChainToGeo3dCircleArraySwelledZLegacyLike(chain,
                                                    /*zScale*/ 1.0,
                                                    TraceMaskExclusionSwellRatioLegacyLike,
                                                    TraceMaskExclusionSwellDiffLegacyLike,
                                                    TraceMaskExclusionSwellLimitLegacyLike);
  if (circles.empty()) {
    return;
  }

  const auto& first = circles.front();
  index.insertSegment(glm::dvec3{first.center[0], first.center[1], first.center[2]},
                      glm::dvec3{first.center[0], first.center[1], first.center[2]},
                      std::max(0.0, first.radius),
                      std::max(0.0, first.radius));

  for (size_t i = 1; i < circles.size(); ++i) {
    const auto& prev = circles[i - 1];
    const auto& cur = circles[i];
    index.insertSegment(glm::dvec3{prev.center[0], prev.center[1], prev.center[2]},
                        glm::dvec3{cur.center[0], cur.center[1], cur.center[2]},
                        std::max(0.0, prev.radius),
                        std::max(0.0, cur.radius));
  }
}

} // namespace

std::vector<std::unique_ptr<LocsegChain>> traceAllSeedsLegacyLike(const ZImg& signal,
                                                                  double zScale,
                                                                  std::vector<LocalNeuroseg>& locsegArray,
                                                                  std::vector<double>& scores,
                                                                  TraceWorkspace& tw)
{
  CHECK(scores.size() == locsegArray.size());
  CHECK(std::isfinite(zScale));
  CHECK(zScale > 0.0);

  const int nseed = static_cast<int>(locsegArray.size());
  if (nseed <= 0) {
    return {};
  }

  tw.resolution = traceResolutionFromZScaleLegacyLike(zScale);

  VLOG(1) << fmt::format("Trace all seeds: start (nseed={}, minScore={})", nseed, tw.minScore);

  std::vector<int> indices;
  darrayQsortLegacy(scores, &indices);
  CHECK(static_cast<int>(indices.size()) == nseed);

  std::shared_ptr<ZSwcSpatialIndex> spatialTraceMaskIndex;

  // Ensure trace mask exists if we're updating it (legacy allocates it lazily here).
  if (tw.traceMaskUpdating) {
    if (FLAGS_atlas_trace_use_swc_geometry_mask) {
      spatialTraceMaskIndex = ensureSpatialTraceMaskVolumeLegacyLike(tw, signal, zScale);
    } else {
      traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ false);
    }
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

    const int seedRank = nseed - i; // 1..nseed in trace order (highest score first)
    if (VLOG_IS_ON(1)) {
      const int reportEvery = 100;
      if (seedRank <= 3 || (seedRank % reportEvery) == 0 || seedRank == nseed) {
        VLOG(1) << fmt::format("Trace all seeds: seed {}/{} (score={:.6f})", seedRank, nseed, seedScore);
      }
    }
    if (VLOG_IS_ON(2)) {
      const std::array<double, 3> center = localNeurosegCenterLegacyLike(seedLocseg);
      VLOG(2) << fmt::format("Trace all seeds: seed {}/{} center=({:.1f}, {:.1f}, {:.1f}) score={:.6f}",
                             seedRank,
                             nseed,
                             center[0],
                             center[1],
                             center[2],
                             seedScore);
    }

    if (!localNeurosegGoodScoreLegacyLike(seedLocseg, seedScore, tw.minScore)) {
      continue;
    }

    const double width = neurosegRBLegacyLike(seedLocseg.seg);
    if (width > tw.dyvar[0]) {
      continue;
    }

    traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::Normal, TraceStatus::Normal);

    if (tw.traceMaskVolume || tw.traceMask) {
      std::array<double, 3> pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h / 3.0);
      if (traceWorkspaceMaskValueZLegacyLike(tw, pt, zScale) > 0) {
        tw.traceStatus[0] = TraceStatus::HitMark;
      }

      pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h * 2.0 / 3.0);
      if (traceWorkspaceMaskValueZLegacyLike(tw, pt, zScale) > 0) {
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

    if (tw.traceMaskUpdating) {
      if (spatialTraceMaskIndex) {
        insertChainIntoSpatialIndexLegacyLike(*chain, *spatialTraceMaskIndex);
      } else if (tw.traceMask) {
        labelWs.sratio = TraceMaskExclusionSwellRatioLegacyLike;
        labelWs.sdiff = TraceMaskExclusionSwellDiffLegacyLike;
        labelWs.slimit = TraceMaskExclusionSwellLimitLegacyLike;
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
    }

    chains.push_back(std::move(chain));
  }

  VLOG(1) << fmt::format("Trace all seeds: done (chains={}).", chains.size());
  return chains;
}

} // namespace nim
