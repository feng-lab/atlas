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

#include <array>
#include <cmath>

DECLARE_bool(atlas_trace_use_swc_geometry_mask);
DECLARE_bool(atlas_autotrace_use_swc_geometry_mask);

DEFINE_double(atlas_trace_mask_exclusion_swell_ratio,
              1.5,
              "Trace-mask exclusion envelope swelling ratio applied when updating the traced-region mask during "
              "seeded/auto tracing. This affects both dense trace-mask labeling and the SWC-geometry mask index. "
              "Increase to be more aggressive (fewer redundant traces, potentially less complete tracing).");

DEFINE_double(
  atlas_trace_mask_exclusion_swell_diff,
  0.0,
  "Trace-mask exclusion envelope swelling additive term applied when updating the traced-region mask during "
  "seeded/auto tracing. This affects both dense trace-mask labeling and the SWC-geometry mask index.");

DEFINE_double(atlas_trace_mask_exclusion_swell_limit,
              3.0,
              "Trace-mask exclusion envelope swelling limit (cap) applied when updating the traced-region mask during "
              "seeded/auto tracing. This affects both dense trace-mask labeling and the SWC-geometry mask index.");

namespace nim {

namespace {

[[nodiscard]] std::shared_ptr<ZSwcSpatialIndex>
ensureSpatialTraceMaskVolumeLegacyLike(TraceWorkspace& tw, const ZImg& signal, double zToXYRatio)
{
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  std::shared_ptr<ZSwcSpatialIndex> index;
  if (tw.traceMaskVolume) {
    auto* spatialMask = dynamic_cast<ZSwcGeometryMaskVolume*>(tw.traceMaskVolume.get());
    CHECK(spatialMask != nullptr)
      << "traceAllSeedsLegacyLike: spatial trace-mask mode requires ZSwcGeometryMaskVolume.";
    index = spatialMask->sharedIndex();
    CHECK(index != nullptr);
    index->setZToXYRatio(zToXYRatio);
    CHECK(spatialMask->width() == signal.width());
    CHECK(spatialMask->height() == signal.height());
    CHECK(spatialMask->depth() == signal.depth());
    return index;
  } else {
    index = std::make_shared<ZSwcSpatialIndex>();
    index->setZToXYRatio(zToXYRatio);
  }

  tw.traceMaskVolume = std::make_unique<ZSwcGeometryMaskVolume>(index, signal.width(), signal.height(), signal.depth());
  return index;
}

void insertChainIntoSpatialIndexLegacyLike(const LocsegChain& chain, ZSwcSpatialIndex& index, double zToXYRatio)
{
  const double swellRatio = FLAGS_atlas_trace_mask_exclusion_swell_ratio;
  const double swellDiff = FLAGS_atlas_trace_mask_exclusion_swell_diff;
  const double swellLimit = FLAGS_atlas_trace_mask_exclusion_swell_limit;
  CHECK(std::isfinite(swellRatio));
  CHECK(std::isfinite(swellDiff));
  CHECK(std::isfinite(swellLimit));

  const std::vector<Geo3dCircle> circles =
    locsegChainToGeo3dCircleArraySwelledZLegacyLike(chain, zToXYRatio, swellRatio, swellDiff, swellLimit);
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

std::vector<std::unique_ptr<LocsegChain>>
traceAllSeedsLegacyLike(const ZImg& signal,
                        double zToXYRatio,
                        std::vector<LocalNeuroseg>& locsegArray,
                        std::vector<double>& scores,
                        TraceWorkspace& tw,
                        std::vector<TraceAllSeedsEndStatusLegacyLike>* outEndStatuses)
{
  CHECK(scores.size() == locsegArray.size());
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);

  const int nseed = static_cast<int>(locsegArray.size());
  if (nseed <= 0) {
    if (outEndStatuses != nullptr) {
      outEndStatuses->clear();
    }
    return {};
  }

  VLOG(1) << fmt::format("Trace all seeds: start (nseed={}, minScore={})", nseed, tw.minScore);

  std::vector<int> indices;
  darrayQsortLegacy(scores, &indices);
  CHECK(static_cast<int>(indices.size()) == nseed);

  std::shared_ptr<ZSwcSpatialIndex> spatialTraceMaskIndex;

  // Ensure trace mask exists if we're updating it (legacy allocates it lazily here).
  if (tw.traceMaskUpdating) {
    const bool useGeometryTraceMask =
      FLAGS_atlas_trace_use_swc_geometry_mask && FLAGS_atlas_autotrace_use_swc_geometry_mask;
    if (useGeometryTraceMask) {
      spatialTraceMaskIndex = ensureSpatialTraceMaskVolumeLegacyLike(tw, signal, zToXYRatio);
    } else {
      traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ false);
    }
  }

  LocsegLabelWorkspaceLegacyLike labelWs;
  labelWs.signal = &signal;

  std::vector<std::unique_ptr<LocsegChain>> chains;
  chains.reserve(static_cast<size_t>(nseed));

  if (outEndStatuses != nullptr) {
    outEndStatuses->clear();
    outEndStatuses->reserve(static_cast<size_t>(nseed));
  }

  struct TraceAllSeedsStats
  {
    size_t visited = 0;
    size_t skippedLowScore = 0;
    size_t skippedTooWide = 0;
    size_t skippedMaskedBothEnds = 0;
    size_t traced = 0;
    size_t kept = 0;
    std::array<size_t, static_cast<size_t>(TraceStatus::NotAssigned) + 1> endStatusHead{};
    std::array<size_t, static_cast<size_t>(TraceStatus::NotAssigned) + 1> endStatusTail{};
  };

  TraceAllSeedsStats stats;

  for (int i = nseed - 1; i >= 0; --i) {
    maybeCancel(tw.cancellationToken);
    ++stats.visited;

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
      ++stats.skippedLowScore;
      continue;
    }

    const double width = neurosegRBLegacyLike(seedLocseg.seg);
    if (width > tw.dyvar[0]) {
      ++stats.skippedTooWide;
      continue;
    }

    traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::Normal, TraceStatus::Normal);

    if (tw.traceMaskVolume || tw.traceMask) {
      std::array<double, 3> pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h / 3.0);
      if (traceWorkspaceMaskValueZLegacyLike(tw, pt, zToXYRatio) > 0) {
        tw.traceStatus[0] = TraceStatus::HitMark;
      }

      pt = localNeurosegAxisPositionLegacyLike(seedLocseg, seedLocseg.seg.h * 2.0 / 3.0);
      if (traceWorkspaceMaskValueZLegacyLike(tw, pt, zToXYRatio) > 0) {
        tw.traceStatus[1] = TraceStatus::HitMark;
      }
    }

    if (tw.traceStatus[0] != TraceStatus::Normal && tw.traceStatus[1] != TraceStatus::Normal) {
      ++stats.skippedMaskedBothEnds;
      continue;
    }

    ++stats.traced;

    TraceRecord tr;
    traceRecordReset(tr);
    traceRecordSetDirection(tr, TraceDirection::BothDir);

    auto chain = std::make_unique<LocsegChain>();
    LocsegNode node;
    node.locseg = seedLocseg;
    node.tr = tr;
    (void)chain->addNode(std::move(node), LocsegChainEndLegacyLike::Tail);

    traceLocsegLegacyLike(signal, zToXYRatio, *chain, tw);
    (void)locsegChainRemoveOverlapEndsLegacyLike(*chain);
    locsegChainRemoveTurnEndsLegacyLike(*chain, 1.0);

    stats.endStatusHead[static_cast<size_t>(tw.traceStatus[0])] += 1;
    stats.endStatusTail[static_cast<size_t>(tw.traceStatus[1])] += 1;

    const bool keep = (locsegChainGeolenLegacyLike(*chain) >= tw.minChainLength) ||
                      (tw.traceStatus[0] == TraceStatus::HitMark) || (tw.traceStatus[1] == TraceStatus::HitMark);
    if (!keep) {
      continue;
    }

    ++stats.kept;

    if (outEndStatuses != nullptr) {
      outEndStatuses->push_back({tw.traceStatus[0], tw.traceStatus[1]});
    }

    if (tw.traceMaskUpdating) {
      if (spatialTraceMaskIndex) {
        insertChainIntoSpatialIndexLegacyLike(*chain, *spatialTraceMaskIndex, zToXYRatio);
      }

      if (tw.traceMask) {
        labelWs.sratio = FLAGS_atlas_trace_mask_exclusion_swell_ratio;
        labelWs.sdiff = FLAGS_atlas_trace_mask_exclusion_swell_diff;
        labelWs.slimit = FLAGS_atlas_trace_mask_exclusion_swell_limit;
        labelWs.option = 1;
        // Atlas uses a binary "already traced" mask (0/1). The migrated algorithm only checks mask voxels as
        // a boolean (>0), so a constant value is sufficient and avoids uint8 overflow edge cases.
        labelWs.value = 1;
        labelWs.flag = 0;
        locsegChainLabelWLegacyLike(*chain,
                                    *tw.traceMask,
                                    zToXYRatio,
                                    /*begin*/ 0,
                                    /*end*/ chain->length() - 1,
                                    labelWs);
      }
    }

    chains.push_back(std::move(chain));
  }

  CHECK(outEndStatuses == nullptr || outEndStatuses->size() == chains.size());

  if (VLOG_IS_ON(1)) {
    const size_t statusHitMarkHead = stats.endStatusHead[static_cast<size_t>(TraceStatus::HitMark)];
    const size_t statusHitMarkTail = stats.endStatusTail[static_cast<size_t>(TraceStatus::HitMark)];
    const size_t statusLowScoreHead = stats.endStatusHead[static_cast<size_t>(TraceStatus::LowScore)];
    const size_t statusLowScoreTail = stats.endStatusTail[static_cast<size_t>(TraceStatus::LowScore)];
    const size_t statusOutOfBoundHead = stats.endStatusHead[static_cast<size_t>(TraceStatus::OutOfBound)];
    const size_t statusOutOfBoundTail = stats.endStatusTail[static_cast<size_t>(TraceStatus::OutOfBound)];

    VLOG(1) << fmt::format(
      "Trace all seeds: stats visited={}, traced={}, kept={}, skippedLowScore={}, skippedTooWide={}, "
      "skippedMaskedBothEnds={} (maskMode={})",
      stats.visited,
      stats.traced,
      stats.kept,
      stats.skippedLowScore,
      stats.skippedTooWide,
      stats.skippedMaskedBothEnds,
      (tw.traceMaskVolume ? "swc_geometry" : (tw.traceMask ? "dense" : "none")));

    VLOG(1) << fmt::format(
      "Trace all seeds: end statuses (head: HitMark={} LowScore={} OutOfBound={} | tail: HitMark={} LowScore={} "
      "OutOfBound={})",
      statusHitMarkHead,
      statusLowScoreHead,
      statusOutOfBoundHead,
      statusHitMarkTail,
      statusLowScoreTail,
      statusOutOfBoundTail);
  }

  VLOG(1) << fmt::format("Trace all seeds: done (chains={}).", chains.size());
  return chains;
}

} // namespace nim
