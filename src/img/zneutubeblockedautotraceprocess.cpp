#include "zneutubeblockedautotraceprocess.h"

#include "zblockedautotracesession.h"

#include "zblockedautotracebounds.h"

#include "zneutubeimgbinarizer.h"
#include "zneutubetraceallseeds.h"
#include "zneutubetracemask.h"
#include "zneutubetracerecover.h"
#include "zneutubetraceseed.h"
#include "zneutubetraceseeder.h"
#include "zneutubetraceconnect.h"
#include "zneutubetracescorethresholds.h"
#include "zneutubetraceswclabelstack.h"
#include "zneutubetraceswclocseg.h"
#include "zneutubetracelocseglabel.h"
#include "zneutubestackfitoptions.h"
#include "zneutubelocsegchainmetrics.h"
#include "zneutubelocsegchaintrace.h"
#include "zneutubemathutils.h"
#include "zneutubetracezscale.h"
#include "zswcgeometrymaskvolume.h"
#include "zswcops.h"
#include "zswcpostprocess.h"
#include "zswcspatialindex.h"
#include "zswcresampler.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zlog.h"

#include <gflags/gflags.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <folly/ScopeGuard.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

DECLARE_bool(atlas_trace_use_swc_geometry_mask);
DECLARE_bool(atlas_autotrace_use_swc_geometry_mask);
DECLARE_double(atlas_trace_mask_exclusion_swell_ratio);
DECLARE_double(atlas_trace_mask_exclusion_swell_diff);
DECLARE_double(atlas_trace_mask_exclusion_swell_limit);

DEFINE_uint32(atlas_autotrace_block_core_x, 1024, "Blocked auto trace: core block size in X (voxels).");
DEFINE_uint32(atlas_autotrace_block_core_y, 1024, "Blocked auto trace: core block size in Y (voxels).");
DEFINE_uint32(atlas_autotrace_block_core_z, 1024, "Blocked auto trace: core block size in Z (voxels).");
DEFINE_uint32(atlas_autotrace_block_halo, 128, "Blocked auto trace: halo/padding size added on each side (voxels).");

DEFINE_string(atlas_autotrace_block_threshold_mode,
              "auto",
              "Blocked auto trace: signal preprocessing threshold mode. "
              "Supported: 'auto' (subtract background per ROI using the legacy neuTube algorithm) or "
              "'fixed' (subtract atlas_autotrace_block_subtract_constant).");

DEFINE_double(
  atlas_autotrace_block_subtract_constant,
  0.0,
  "Blocked auto trace: fixed threshold/background value to subtract from the signal in each ROI before "
  "mask/seed detection. 0 means no subtraction. Used only when atlas_autotrace_block_threshold_mode='fixed'.");

namespace nim {

namespace {

constexpr int64_t kBlockedAutoTraceMinCoreVoxels = 1024;
constexpr int64_t kBlockedAutoTraceMinHaloVoxels = 128;

[[nodiscard]] std::string normalizeThresholdModeOrThrow(std::string_view mode)
{
  std::string out;
  out.reserve(mode.size());
  for (char c : mode) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  if (out == "auto") {
    return out;
  }
  if (out == "fixed") {
    return out;
  }

  throw ZException(
    fmt::format("Blocked auto trace: invalid atlas_autotrace_block_threshold_mode='{}' (expected 'auto' or 'fixed').",
                std::string(mode)));
}

[[nodiscard]] std::string_view traceDirectionToStringForLog(TraceDirection d)
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

void rescaleSwcInPlace(ZSwc& tree, double scaleX, double scaleY, double scaleZ, bool scaleRadius)
{
  if (scaleX == 1.0 && scaleY == 1.0 && scaleZ == 1.0) {
    return;
  }

  const double radiusScale = std::sqrt(scaleX * scaleY);

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    it->x *= scaleX;
    it->y *= scaleY;
    it->z *= scaleZ;
    if (scaleRadius) {
      it->radius *= radiusScale;
    }
  }
}

struct BlockGrid
{
  ZBlockedAutoTraceDatasetShape shape{};
  ZBlockedAutoTraceBlockSize block{};

  [[nodiscard]] int64_t numBlocksX() const
  {
    CHECK(block.coreX > 0);
    return (shape.width + block.coreX - 1) / block.coreX;
  }

  [[nodiscard]] int64_t numBlocksY() const
  {
    CHECK(block.coreY > 0);
    return (shape.height + block.coreY - 1) / block.coreY;
  }

  [[nodiscard]] int64_t numBlocksZ() const
  {
    CHECK(block.coreZ > 0);
    return (shape.depth + block.coreZ - 1) / block.coreZ;
  }

  [[nodiscard]] uint64_t totalBlocks() const
  {
    const int64_t nx = numBlocksX();
    const int64_t ny = numBlocksY();
    const int64_t nz = numBlocksZ();
    CHECK(nx >= 0 && ny >= 0 && nz >= 0);
    return static_cast<uint64_t>(nx) * static_cast<uint64_t>(ny) * static_cast<uint64_t>(nz);
  }

  [[nodiscard]] uint64_t linearIndexOrThrow(const ZBlockedAutoTraceBlockId& id) const
  {
    const int64_t nx = numBlocksX();
    const int64_t ny = numBlocksY();
    const int64_t nz = numBlocksZ();
    if (id.bx < 0 || id.by < 0 || id.bz < 0 || id.bx >= nx || id.by >= ny || id.bz >= nz) {
      throw ZException(fmt::format("Blocked auto trace: invalid block id ({},{},{})", id.bx, id.by, id.bz));
    }
    return static_cast<uint64_t>(id.bx) + static_cast<uint64_t>(id.by) * static_cast<uint64_t>(nx) +
           static_cast<uint64_t>(id.bz) * static_cast<uint64_t>(nx) * static_cast<uint64_t>(ny);
  }

  [[nodiscard]] bool containsBlockId(const ZBlockedAutoTraceBlockId& id) const
  {
    const int64_t nx = numBlocksX();
    const int64_t ny = numBlocksY();
    const int64_t nz = numBlocksZ();
    return id.bx >= 0 && id.by >= 0 && id.bz >= 0 && id.bx < nx && id.by < ny && id.bz < nz;
  }

  [[nodiscard]] ZBlockedAutoTraceBlockId blockIdFromLinearOrThrow(uint64_t idx) const
  {
    const uint64_t nx = static_cast<uint64_t>(numBlocksX());
    const uint64_t ny = static_cast<uint64_t>(numBlocksY());
    const uint64_t nz = static_cast<uint64_t>(numBlocksZ());
    const uint64_t total = nx * ny * nz;
    if (idx >= total) {
      throw ZException(fmt::format("Blocked auto trace: linear block index {} out of range (total={})", idx, total));
    }

    const uint64_t plane = nx * ny;
    const uint64_t bz = idx / plane;
    const uint64_t rem = idx - bz * plane;
    const uint64_t by = rem / nx;
    const uint64_t bx = rem - by * nx;
    return ZBlockedAutoTraceBlockId{.bx = static_cast<int64_t>(bx),
                                    .by = static_cast<int64_t>(by),
                                    .bz = static_cast<int64_t>(bz)};
  }

  struct Bounds
  {
    int64_t minX = 0;
    int64_t minY = 0;
    int64_t minZ = 0;
    int64_t maxX = 0; // exclusive
    int64_t maxY = 0;
    int64_t maxZ = 0;
  };

  [[nodiscard]] static std::string boundsToString(const Bounds& b)
  {
    return fmt::format("[{}:{})x[{}:{})x[{}:{})", b.minX, b.maxX, b.minY, b.maxY, b.minZ, b.maxZ);
  }

  [[nodiscard]] Bounds coreBounds(const ZBlockedAutoTraceBlockId& id) const
  {
    Bounds b;
    b.minX = id.bx * block.coreX;
    b.minY = id.by * block.coreY;
    b.minZ = id.bz * block.coreZ;
    b.maxX = std::min<int64_t>(shape.width, b.minX + block.coreX);
    b.maxY = std::min<int64_t>(shape.height, b.minY + block.coreY);
    b.maxZ = std::min<int64_t>(shape.depth, b.minZ + block.coreZ);
    return b;
  }

  [[nodiscard]] Bounds roiBounds(const Bounds& core) const
  {
    Bounds r;
    r.minX = std::max<int64_t>(0, core.minX - block.halo);
    r.minY = std::max<int64_t>(0, core.minY - block.halo);
    r.minZ = std::max<int64_t>(0, core.minZ - block.halo);
    r.maxX = std::min<int64_t>(shape.width, core.maxX + block.halo);
    r.maxY = std::min<int64_t>(shape.height, core.maxY + block.halo);
    r.maxZ = std::min<int64_t>(shape.depth, core.maxZ + block.halo);
    return r;
  }

  [[nodiscard]] static std::array<double, 3>
  taskAnchorPoint(const LocalNeuroseg& endLocseg, TraceDirection direction, double zToXYRatio)
  {
    std::array<double, 3> p = localNeurosegCenterLegacyLike(endLocseg);
    if (direction == TraceDirection::Forward) {
      p = localNeurosegTopLegacyLike(endLocseg);
    } else if (direction == TraceDirection::Backward) {
      p = localNeurosegBottomLegacyLike(endLocseg);
    }
    p[2] /= zToXYRatio;
    return p;
  }

  [[nodiscard]] ZBlockedAutoTraceBlockId
  suggestedBlockForTask(const LocalNeuroseg& endLocseg, TraceDirection direction, double zToXYRatio) const
  {
    const std::array<double, 3> p = taskAnchorPoint(endLocseg, direction, zToXYRatio);

    const int64_t x = static_cast<int64_t>(std::floor(p[0]));
    const int64_t y = static_cast<int64_t>(std::floor(p[1]));
    const int64_t z = static_cast<int64_t>(std::floor(p[2]));

    auto clampBlock = [](int64_t v, int64_t core, int64_t nblocks) -> int64_t {
      if (core <= 0 || nblocks <= 0) {
        return 0;
      }
      int64_t b = v / core;
      if (b < 0) {
        b = 0;
      }
      if (b >= nblocks) {
        b = nblocks - 1;
      }
      return b;
    };

    return ZBlockedAutoTraceBlockId{.bx = clampBlock(x, block.coreX, numBlocksX()),
                                    .by = clampBlock(y, block.coreY, numBlocksY()),
                                    .bz = clampBlock(z, block.coreZ, numBlocksZ())};
  }
};

[[nodiscard]] Geo3dScalarField filterSeedsToCoreLegacyLike(Geo3dScalarField seeds,
                                                           const BlockGrid::Bounds& roi,
                                                           const BlockGrid::Bounds& core,
                                                           const folly::CancellationToken& cancellationToken,
                                                           /*nullable*/ size_t* skippedHaloOut = nullptr)
{
  if (skippedHaloOut != nullptr) {
    *skippedHaloOut = 0;
  }
  if (seeds.points.empty()) {
    return seeds;
  }

  Geo3dScalarField keptSeeds;
  keptSeeds.points.reserve(seeds.points.size());
  keptSeeds.values.reserve(seeds.values.size());

  constexpr size_t kCancelCheckEvery = 1u << 20;
  size_t untilCheck = kCancelCheckEvery;
  size_t skippedHalo = 0;

  for (size_t i = 0; i < seeds.points.size(); ++i) {
    if (--untilCheck == 0) {
      maybeCancel(cancellationToken);
      untilCheck = kCancelCheckEvery;
    }

    const int64_t lx = static_cast<int64_t>(seeds.points[i][0]);
    const int64_t ly = static_cast<int64_t>(seeds.points[i][1]);
    const int64_t lz = static_cast<int64_t>(seeds.points[i][2]);

    const int64_t gx = lx + roi.minX;
    const int64_t gy = ly + roi.minY;
    const int64_t gz = lz + roi.minZ;

    if (gx < core.minX || gy < core.minY || gz < core.minZ || gx >= core.maxX || gy >= core.maxY || gz >= core.maxZ) {
      ++skippedHalo;
      continue;
    }

    keptSeeds.points.push_back({static_cast<double>(lx), static_cast<double>(ly), static_cast<double>(lz)});
    keptSeeds.values.push_back(seeds.values[i]);
  }

  if (skippedHaloOut != nullptr) {
    *skippedHaloOut = skippedHalo;
  }
  return keptSeeds;
}

[[nodiscard]] Geo3dScalarField
postProcessBlockedRecoverySeedsLegacyLike(Geo3dScalarField seeds,
                                          ZImg& leftoverMask,
                                          int seedMethod,
                                          const BlockGrid::Bounds& roi,
                                          const BlockGrid::Bounds& core,
                                          const folly::CancellationToken& cancellationToken)
{
  seeds = removeNoisySeedLegacyLike(std::move(seeds),
                                    leftoverMask,
                                    seedMethod,
                                    /*screeningSeed*/ true,
                                    cancellationToken,
                                    nullptr);
  maybeCancel(cancellationToken);
  return filterSeedsToCoreLegacyLike(std::move(seeds), roi, core, cancellationToken);
}

[[nodiscard]] double swellRadiusForExclusionMaskLegacyLike(double r)
{
  if (!std::isfinite(r) || r <= 0.0) {
    return 0.0;
  }

  const double ratio = FLAGS_atlas_trace_mask_exclusion_swell_ratio;
  const double diff = FLAGS_atlas_trace_mask_exclusion_swell_diff;
  const double limit = FLAGS_atlas_trace_mask_exclusion_swell_limit;
  CHECK(std::isfinite(ratio));
  CHECK(std::isfinite(diff));
  CHECK(std::isfinite(limit));

  double out = r * ratio + diff;
  if (limit > 0.0) {
    out = std::min(out, r + limit);
  }

  if (!std::isfinite(out) || out <= 0.0) {
    return 0.0;
  }
  return out;
}

void labelSwcIndexIntoRoiDenseTraceMaskForRecoveryLegacyLike(const ZSwcSpatialIndex& swcIndex,
                                                             ZImg& traceMask,
                                                             const BlockGrid::Bounds& roi,
                                                             double zToXYRatio)
{
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);
  if (traceMask.isEmpty()) {
    return;
  }
  CHECK(traceMask.numChannels() == 1);
  CHECK(traceMask.numTimes() == 1);
  CHECK(traceMask.isType<uint8_t>()) << traceMask.info();

  const double indexZToXYRatio = swcIndex.zToXYRatio();
  CHECK(std::abs(indexZToXYRatio - zToXYRatio) < 1e-9)
    << "labelSwcIndexIntoRoiDenseTraceMaskForRecoveryLegacyLike: swcIndex.zToXYRatio=" << indexZToXYRatio
    << " does not match zToXYRatio=" << zToXYRatio;

  constexpr int kMaskValue = 1;

  size_t labeledSegments = 0;

  const int64_t roiW = roi.maxX - roi.minX;
  const int64_t roiH = roi.maxY - roi.minY;
  const int64_t roiD = roi.maxZ - roi.minZ;
  CHECK(roiW >= 0);
  CHECK(roiH >= 0);
  CHECK(roiD >= 0);
  CHECK(static_cast<size_t>(roiW) == traceMask.width());
  CHECK(static_cast<size_t>(roiH) == traceMask.height());
  CHECK(static_cast<size_t>(roiD) == traceMask.depth());

  const glm::dvec3 minCorner{static_cast<double>(roi.minX),
                             static_cast<double>(roi.minY),
                             static_cast<double>(roi.minZ)};
  const glm::dvec3 maxCorner{static_cast<double>(roi.maxX),
                             static_cast<double>(roi.maxY),
                             static_cast<double>(roi.maxZ)};

  const std::vector<ZSwcSpatialIndex::Segment> segments = swcIndex.querySegmentsIntersectingBox(minCorner, maxCorner);

  LocsegLabelWorkspaceLegacyLike labelWs;
  labelWs.option = 1;
  labelWs.value = kMaskValue;
  labelWs.flag = 0;
  labelWs.sratio = FLAGS_atlas_trace_mask_exclusion_swell_ratio;
  labelWs.sdiff = FLAGS_atlas_trace_mask_exclusion_swell_diff;
  labelWs.slimit = FLAGS_atlas_trace_mask_exclusion_swell_limit;

  const double roiMinX = static_cast<double>(roi.minX);
  const double roiMinY = static_cast<double>(roi.minY);
  const double roiMinZ = static_cast<double>(roi.minZ);
  const double roiMinTraceZ = roiMinZ * zToXYRatio;

  for (const auto& seg : segments) {
    const glm::dvec3 ab = seg.b - seg.a;
    const double ab2 = glm::dot(ab, ab);
    if (ab2 <= 1e-12) {
      const double r = swellRadiusForExclusionMaskLegacyLike(seg.ra);
      if (r <= 0.0) {
        continue;
      }
      geo3dBallLabelStackLegacyLike({seg.a.x - roiMinX, seg.a.y - roiMinY, seg.a.z - roiMinZ},
                                    r,
                                    traceMask,
                                    kMaskValue);
      ++labeledSegments;
      continue;
    }

    LocalNeuroseg locseg;
    locseg.pos = {seg.a.x - roiMinX, seg.a.y - roiMinY, seg.a.z * zToXYRatio - roiMinTraceZ};
    const std::array<double, 3> top = {seg.b.x - roiMinX, seg.b.y - roiMinY, seg.b.z * zToXYRatio - roiMinTraceZ};
    localNeurosegChangeTopLegacyLike(locseg, top);

    locseg.seg.r1 = seg.ra;
    locseg.seg.scale = 1.0;

    const double adjustedHeight = locseg.seg.h - 1.0;
    if (adjustedHeight < 0.001) {
      locseg.seg.c = 1.0;
    } else {
      locseg.seg.c = (seg.rb - seg.ra) / adjustedHeight;
    }

    locseg.seg.alpha = 0.0;
    locseg.seg.curvature = 0.0;

    localNeurosegLabelWLegacyLike(locseg, traceMask, zToXYRatio, labelWs);
    ++labeledSegments;
  }

  VLOG(1) << fmt::format("Blocked auto trace: prefilled ROI dense trace mask from SWC index "
                         "(segmentsLabeled={}, roi={}).",
                         labeledSegments,
                         BlockGrid::boundsToString(roi));
}

[[nodiscard]] bool pointInBoundsForVoxelSampling(const BlockGrid::Bounds& b, const std::array<double, 3>& p)
{
  // Mirrors legacy trace workspace bounds semantics:
  //   min is inclusive, max is inclusive on voxel indices.
  if (b.maxX <= b.minX || b.maxY <= b.minY || b.maxZ <= b.minZ) {
    return false;
  }
  const double maxX = static_cast<double>(b.maxX) - 1.0;
  const double maxY = static_cast<double>(b.maxY) - 1.0;
  const double maxZ = static_cast<double>(b.maxZ) - 1.0;
  return p[0] >= static_cast<double>(b.minX) && p[1] >= static_cast<double>(b.minY) &&
         p[2] >= static_cast<double>(b.minZ) && p[0] <= maxX && p[1] <= maxY && p[2] <= maxZ;
}

[[nodiscard]] double locsegNodeRadiusForSwc(const LocalNeuroseg& locseg);

struct ContinuationHint
{
  std::array<double, 3> anchor{};
  std::array<double, 3> axisDir{};
  double step = 0.0;
  double radiusXY = 0.0;
  double radiusZ = 0.0;
};

[[nodiscard]] ContinuationHint
continuationHintForTask(const LocalNeuroseg& endGlobal, TraceDirection direction, double zToXYRatio)
{
  ContinuationHint hint;

  auto top = localNeurosegTopLegacyLike(endGlobal);
  auto bottom = localNeurosegBottomLegacyLike(endGlobal);
  top[2] /= zToXYRatio;
  bottom[2] /= zToXYRatio;

  if (direction == TraceDirection::Forward) {
    hint.anchor = top;
    hint.axisDir = {top[0] - bottom[0], top[1] - bottom[1], top[2] - bottom[2]};
  } else {
    hint.anchor = bottom;
    hint.axisDir = {bottom[0] - top[0], bottom[1] - top[1], bottom[2] - top[2]};
  }

  const double axisLen = std::sqrt(hint.axisDir[0] * hint.axisDir[0] + hint.axisDir[1] * hint.axisDir[1] +
                                   hint.axisDir[2] * hint.axisDir[2]);
  hint.step = std::max(1.0, axisLen * 0.5);

  hint.radiusXY = std::max(0.0, locsegNodeRadiusForSwc(endGlobal));
  hint.radiusZ = hint.radiusXY;
  return hint;
}

[[nodiscard]] std::array<double, 3> continuationProbePoint(const ContinuationHint& hint)
{
  const double len2 =
    hint.axisDir[0] * hint.axisDir[0] + hint.axisDir[1] * hint.axisDir[1] + hint.axisDir[2] * hint.axisDir[2];
  if (!(hint.step > 0.0) || !(len2 > 0.0)) {
    return hint.anchor;
  }

  const double invLen = 1.0 / std::sqrt(len2);
  return {hint.anchor[0] + hint.axisDir[0] * invLen * hint.step,
          hint.anchor[1] + hint.axisDir[1] * invLen * hint.step,
          hint.anchor[2] + hint.axisDir[2] * invLen * hint.step};
}

[[nodiscard]] std::array<double, 3> continuationPointAtDistance(const ContinuationHint& hint, double distance)
{
  const double len2 =
    hint.axisDir[0] * hint.axisDir[0] + hint.axisDir[1] * hint.axisDir[1] + hint.axisDir[2] * hint.axisDir[2];
  if (!(distance > 0.0) || !(len2 > 0.0)) {
    return hint.anchor;
  }

  const double invLen = 1.0 / std::sqrt(len2);
  return {hint.anchor[0] + hint.axisDir[0] * invLen * distance,
          hint.anchor[1] + hint.axisDir[1] * invLen * distance,
          hint.anchor[2] + hint.axisDir[2] * invLen * distance};
}

[[nodiscard]] int64_t maxSwcNodeIdOrZero(const ZSwc& swc)
{
  int64_t maxId = 0;
  for (auto it = swc.cbegin(); it != swc.cend(); ++it) {
    maxId = std::max<int64_t>(maxId, it->id);
  }
  return maxId;
}

[[nodiscard]] uint64_t maxTaskIdOrZero(const std::vector<ZBlockedAutoTracePendingTask>& tasks)
{
  uint64_t maxId = 0;
  for (const auto& t : tasks) {
    maxId = std::max<uint64_t>(maxId, t.taskId);
  }
  return maxId;
}

[[nodiscard]] double locsegNodeRadiusForSwc(const LocalNeuroseg& locseg)
{
  // Use the "equivalent XY radius" at the middle of the segment as a stable scalar radius for SWC.
  // This aligns with the geometry hit-test model (tapered segments with per-node radii).
  const double z = (locseg.seg.h <= 0.0) ? 0.0 : (locseg.seg.h - 1.0) * 0.5;
  const double r = neurosegRxyZLegacyLike(locseg.seg, z);
  if (!std::isfinite(r) || r <= 0.0) {
    return 0.0;
  }
  return r;
}

[[nodiscard]] ZBlockedAutoTraceSwcDeltaNode
makeDeltaNode(int64_t id, int64_t parentId, const LocalNeuroseg& locseg, glm::dvec3 origin, double zToXYRatio)
{
  std::array<double, 3> c = localNeurosegCenterLegacyLike(locseg);
  c[2] /= zToXYRatio;
  ZBlockedAutoTraceSwcDeltaNode d;
  d.id = id;
  d.type = 0;
  d.x = c[0] + origin.x;
  d.y = c[1] + origin.y;
  d.z = c[2] + origin.z;
  d.radius = locsegNodeRadiusForSwc(locseg);
  d.parentId = parentId;
  return d;
}

[[nodiscard]] ZBlockedAutoTraceSwcDeltaNode makeDeltaNodeFromSwcNode(ZSwc::SwcTreeNode node)
{
  CHECK(!ZSwc::isNull(node));
  ZBlockedAutoTraceSwcDeltaNode d;
  d.id = node->id;
  d.type = node->type;
  d.x = node->x;
  d.y = node->y;
  d.z = node->z;
  d.radius = node->radius;
  if (const auto parent = ZSwc::parent(node); !ZSwc::isNull(parent)) {
    d.parentId = parent->id;
  } else {
    d.parentId = -1;
  }
  return d;
}

void appendDeltaNodeToSwcOrThrow(const ZBlockedAutoTraceSwcDeltaNode& d,
                                 ZSwc& swc,
                                 std::unordered_map<int64_t, ZSwc::SwcTreeNode>& nodeById)
{
  if (d.id <= 0) {
    throw ZException(fmt::format("Blocked auto trace: invalid SWC node id {}", d.id));
  }
  if (nodeById.contains(d.id)) {
    throw ZException(fmt::format("Blocked auto trace: duplicate SWC node id {}", d.id));
  }

  SwcNode node(d.id, d.type, d.x, d.y, d.z, d.radius, d.parentId);
  auto it = swc.appendRoot(node);
  nodeById.emplace(d.id, it);

  if (d.parentId >= 0) {
    auto pit = nodeById.find(d.parentId);
    if (pit == nodeById.end()) {
      throw ZException(fmt::format("Blocked auto trace: missing parent id {} for new node {}", d.parentId, d.id));
    }
    swc.appendChild(pit->second, it);
  }
}

void writeSwcAtomicKeepIdsOrThrow(const ZSwc& tree, const QString& outPath)
{
  if (outPath.isEmpty()) {
    throw ZException("Blocked auto trace: output SWC path is empty.");
  }

  const QDir dir = QFileInfo(outPath).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    throw ZException(QStringLiteral("Blocked auto trace: can not create output directory: %1").arg(dir.absolutePath()));
  }

  const QString tmpPath = outPath + QStringLiteral(".tmp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto tmpGuard = folly::makeGuard([&]() {
    (void)QFile::remove(tmpPath);
  });

  tree.save(tmpPath);

  if (QFile::exists(outPath) && !QFile::remove(outPath)) {
    throw ZException(QStringLiteral("Blocked auto trace: can not overwrite output SWC: %1").arg(outPath));
  }
  if (!QFile::rename(tmpPath, outPath)) {
    throw ZException(QStringLiteral("Blocked auto trace: can not move temp SWC into place.\nTemp: %1\nFinal: %2")
                       .arg(tmpPath, outPath));
  }

  tmpGuard.dismiss();
}

} // namespace

TraceConfig ZNeutubeBlockedAutoTraceProcess::buildEffectiveTraceConfigOrThrow() const
{
  TraceConfig cfg;

  if (!m_traceConfigPath.isEmpty()) {
    const bool ok = loadTraceConfigLegacyLike(m_traceConfigPath.toStdString(), cfg);
    if (!ok) {
      cfg = TraceConfig{};
    }
  }

  if (m_traceLevel > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(cfg, m_traceLevel)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, cfg);
    }
  }

  if (m_haveAlgoOverrides) {
    cfg.minAutoScore = m_algoOverrides.minAutoScore;
    cfg.minManualScore = m_algoOverrides.minManualScore;
    cfg.minSeedScore = m_algoOverrides.minSeedScore;
    cfg.min2dScore = m_algoOverrides.min2dScore;
    cfg.refit = m_algoOverrides.refit;
    cfg.spTest = m_algoOverrides.spTest;
    cfg.crossoverTest = m_algoOverrides.crossoverTest;
    cfg.tuneEnd = m_algoOverrides.tuneEnd;
    cfg.edgePath = m_algoOverrides.edgePath;
    cfg.enhanceMask = m_algoOverrides.enhanceMask;
    cfg.seedMethod = m_algoOverrides.seedMethod;
    cfg.recover = m_algoOverrides.recover;
    cfg.chainScreenCount = m_algoOverrides.chainScreenCount;
    cfg.maxEucDist = m_algoOverrides.maxEucDist;
  }

  if (m_docHasAnySwc) {
    // Preserve legacy UI semantics: disable "recover" when an SWC already exists in the doc.
    cfg.recover = 0;
  }

  return cfg;
}

void ZNeutubeBlockedAutoTraceProcess::writeFinalSwcAtomicOrThrow(ZSwc& tree) const
{
  if (m_outputSwcPath.isEmpty()) {
    throw ZException("Blocked auto trace: output SWC path is empty.");
  }

  resortId(tree);
  swcTreeRemoveZigzagLegacyLike(tree);
  swcTreeTuneBranchLegacyLike(tree);
  swcTreeRemoveSpurLegacyLike(tree);
  swcTreeMergeCloseNodeLegacyLike(tree, /*threshold*/ 0.01);
  swcTreeRemoveOvershootLegacyLike(tree);

  // Session SWC must remain append-only and ID-stable for resume correctness.
  // Apply postprocessing (e.g. resampling) only to the final output artifact.
  if (m_doResampleAfterTracing) {
    ZNeutubeSwcResampler resampler;
    resampler.optimalDownsample(tree);
  }

  swcTreeRemoveOrphanBlobLegacyLike(tree, /*minLength*/ 0.0, /*minOrphanCount*/ 10);
  resortId(tree);

  if (m_signalDownsampleRatio != std::array<size_t, 3>{1, 1, 1}) {
    rescaleSwcInPlace(tree,
                      static_cast<double>(m_signalDownsampleRatio[0]),
                      static_cast<double>(m_signalDownsampleRatio[1]),
                      static_cast<double>(m_signalDownsampleRatio[2]),
                      /*scaleRadius=*/true);
  }

  writeSwcAtomicKeepIdsOrThrow(tree, m_outputSwcPath);
}

void ZNeutubeBlockedAutoTraceProcess::doWork()
{
  m_hasResult = false;

  if (!m_roiProvider) {
    throw ZException("Blocked auto trace: missing ROI signal provider.");
  }
  if (m_signalInfo.isEmpty()) {
    throw ZException("Blocked auto trace: missing/empty signal info.");
  }
  if (m_outputSessionDir.isEmpty()) {
    throw ZException("Blocked auto trace: output session dir is empty.");
  }
  if (m_datasetId.empty()) {
    throw ZException("Blocked auto trace: dataset identity is empty.");
  }
  if (!m_zToXYRatio.has_value()) {
    throw ZException("Blocked auto trace: missing zToXYRatio.");
  }

  if (m_signalDownsampleRatio[0] != m_signalDownsampleRatio[1]) {
    throw ZException(fmt::format("Blocked auto trace: XY downsample ratio must be uniform, got ratio=[{},{},{}].",
                                 m_signalDownsampleRatio[0],
                                 m_signalDownsampleRatio[1],
                                 m_signalDownsampleRatio[2]));
  }

  const TraceConfig cfg = buildEffectiveTraceConfigOrThrow();

  const std::array<size_t, 3> ratio = m_signalDownsampleRatio;
  const int64_t baseW = static_cast<int64_t>(m_signalInfo.width);
  const int64_t baseH = static_cast<int64_t>(m_signalInfo.height);
  const int64_t baseD = static_cast<int64_t>(m_signalInfo.depth);

  const int64_t traceW = (baseW + static_cast<int64_t>(ratio[0]) - 1) / static_cast<int64_t>(ratio[0]);
  const int64_t traceH = (baseH + static_cast<int64_t>(ratio[1]) - 1) / static_cast<int64_t>(ratio[1]);
  const int64_t traceD = (baseD + static_cast<int64_t>(ratio[2]) - 1) / static_cast<int64_t>(ratio[2]);

  LOG(INFO) << "Atlas Blocked Auto Trace";
  LOG(INFO) << "Selected channel (0-based): " << m_selectedChannel;
  LOG(INFO) << "Selected time (0-based): " << m_selectedTime;
  LOG(INFO) << "Signal downsample ratio: [" << ratio[0] << "," << ratio[1] << "," << ratio[2] << "]";
  LOG(INFO) << fmt::format("Tracing zToXYRatio: {:.6g}", *m_zToXYRatio);
  LOG(INFO) << "Budget level override (0=default): " << m_traceLevel;
  LOG(INFO) << "Optimal node resampling: " << (m_doResampleAfterTracing ? "enabled" : "disabled");
  LOG(INFO) << "Trace config path: " << m_traceConfigPath;
  LOG(INFO) << "Output session dir: " << m_outputSessionDir;
  LOG(INFO) << "Output SWC: " << m_outputSwcPath;

  LOG(INFO) << "Final TraceConfig:";
  LOG(INFO) << "  minAutoScore=" << cfg.minAutoScore;
  LOG(INFO) << "  minManualScore=" << cfg.minManualScore;
  LOG(INFO) << "  minSeedScore=" << cfg.minSeedScore;
  LOG(INFO) << "  min2dScore=" << cfg.min2dScore;
  LOG(INFO) << "  refit=" << (cfg.refit ? "true" : "false");
  LOG(INFO) << "  spTest=" << (cfg.spTest ? "true" : "false");
  LOG(INFO) << "  crossoverTest=" << (cfg.crossoverTest ? "true" : "false");
  LOG(INFO) << "  tuneEnd=" << (cfg.tuneEnd ? "true" : "false");
  LOG(INFO) << "  edgePath=" << (cfg.edgePath ? "true" : "false");
  LOG(INFO) << "  enhanceMask=" << (cfg.enhanceMask ? "true" : "false");
  LOG(INFO) << "  seedMethod=" << cfg.seedMethod;
  LOG(INFO) << "  recover=" << cfg.recover;
  LOG(INFO) << "  chainScreenCount=" << cfg.chainScreenCount;
  LOG(INFO) << "  maxEucDist=" << cfg.maxEucDist;

  ZBlockedAutoTraceManifest manifest;
  manifest.formatVersion = kBlockedAutoTraceManifestFormatVersion;
  manifest.datasetId = m_datasetId;
  manifest.channel = m_selectedChannel;
  manifest.time = m_selectedTime;
  manifest.signalDownsampleRatio = ratio;
  manifest.zToXYRatio = *m_zToXYRatio;
  manifest.datasetShape = {
    .width = traceW,
    .height = traceH,
    .depth = traceD,
  };

  const int64_t reqCoreX = m_blockCoreX.value_or(static_cast<int64_t>(FLAGS_atlas_autotrace_block_core_x));
  const int64_t reqCoreY = m_blockCoreY.value_or(static_cast<int64_t>(FLAGS_atlas_autotrace_block_core_y));
  const int64_t reqCoreZ = m_blockCoreZ.value_or(static_cast<int64_t>(FLAGS_atlas_autotrace_block_core_z));
  const int64_t reqHalo = m_blockHalo.value_or(static_cast<int64_t>(FLAGS_atlas_autotrace_block_halo));

  manifest.block = {
    .coreX = reqCoreX,
    .coreY = reqCoreY,
    .coreZ = reqCoreZ,
    .halo = reqHalo,
  };
  manifest.thresholdMode = normalizeThresholdModeOrThrow(FLAGS_atlas_autotrace_block_threshold_mode);
  if (manifest.thresholdMode == "fixed") {
    manifest.subtractConstant = FLAGS_atlas_autotrace_block_subtract_constant;
  } else if (manifest.thresholdMode == "auto") {
    if (FLAGS_atlas_autotrace_block_subtract_constant != 0.0) {
      LOG(WARNING) << fmt::format(
        "Blocked auto trace: atlas_autotrace_block_subtract_constant={} is ignored because threshold_mode='auto'.",
        FLAGS_atlas_autotrace_block_subtract_constant);
    }
    manifest.subtractConstant = 0.0;
  } else {
    CHECK(false) << fmt::format("Unexpected thresholdMode: '{}'", manifest.thresholdMode);
  }
  manifest.traceConfig = cfg;

  if (manifest.block.coreX <= 0 || manifest.block.coreY <= 0 || manifest.block.coreZ <= 0 || manifest.block.halo < 0) {
    throw ZException(fmt::format("Blocked auto trace: invalid block config core=({}, {}, {}), halo={}.",
                                 manifest.block.coreX,
                                 manifest.block.coreY,
                                 manifest.block.coreZ,
                                 manifest.block.halo));
  }
  if (manifest.block.coreX < kBlockedAutoTraceMinCoreVoxels || manifest.block.coreY < kBlockedAutoTraceMinCoreVoxels ||
      manifest.block.coreZ < kBlockedAutoTraceMinCoreVoxels) {
    throw ZException(fmt::format("Blocked auto trace: block core size too small.\n"
                                 "  requestedCore=({}, {}, {})\n"
                                 "  hardMinimum={}",
                                 manifest.block.coreX,
                                 manifest.block.coreY,
                                 manifest.block.coreZ,
                                 kBlockedAutoTraceMinCoreVoxels));
  }
  if (manifest.block.halo < kBlockedAutoTraceMinHaloVoxels) {
    throw ZException(fmt::format("Blocked auto trace: halo/padding too small.\n"
                                 "  requestedHalo={}\n"
                                 "  hardMinimum={}",
                                 manifest.block.halo,
                                 kBlockedAutoTraceMinHaloVoxels));
  }

  ZBlockedAutoTraceSession session(m_outputSessionDir);

  // If resuming an existing session, treat the persisted manifest as source-of-truth for preprocessing policy so the
  // GUI can resume without requiring users to re-pass gflags.
  const QString existingManifestPath = QDir(session.sessionDir()).absoluteFilePath(QStringLiteral("manifest.json"));
  if (QFileInfo::exists(existingManifestPath)) {
    const ZBlockedAutoTraceManifest existing = session.loadManifestOrThrow();
    if (existing.thresholdMode != manifest.thresholdMode) {
      LOG(WARNING) << fmt::format(
        "Blocked auto trace: ignoring requested threshold_mode='{}' and resuming with manifest threshold_mode='{}'.",
        manifest.thresholdMode,
        existing.thresholdMode);
    }
    if (existing.subtractConstant != manifest.subtractConstant) {
      LOG(WARNING) << fmt::format(
        "Blocked auto trace: ignoring requested subtract_constant={} and resuming with manifest subtract_constant={}.",
        manifest.subtractConstant,
        existing.subtractConstant);
    }
    manifest.thresholdMode = existing.thresholdMode;
    manifest.subtractConstant = existing.subtractConstant;
  }

  session.ensureCreatedOrThrow(manifest);

  ZBlockedAutoTraceLoadedState state = session.loadLatestOrEmptyOrThrow();
  CHECK(state.manifest.formatVersion == kBlockedAutoTraceManifestFormatVersion);
  // Build in-memory state.
  int64_t nextNodeId = maxSwcNodeIdOrZero(state.swc) + 1;
  uint64_t nextTaskId = maxTaskIdOrZero(state.frontier) + 1;

  BlockGrid grid;
  grid.shape = state.manifest.datasetShape;
  grid.block = state.manifest.block;

  const uint64_t totalBlocks = grid.totalBlocks();
  std::unordered_set<uint64_t> seedScanned;
  seedScanned.reserve(state.seedScannedBlocks.size() * 2 + 1);
  for (const auto& bid : state.seedScannedBlocks) {
    seedScanned.insert(grid.linearIndexOrThrow(bid));
  }

  auto advanceLinearScanCursor = [&]() {
    uint64_t cur = state.scheduler.nextLinearBlockIndex;
    while (cur < totalBlocks && seedScanned.contains(cur)) {
      ++cur;
    }
    state.scheduler.nextLinearBlockIndex = cur;
  };

  // Ensure the cursor is consistent when resuming sessions created by older builds.
  if (state.scheduler.nextLinearBlockIndex >= totalBlocks) {
    state.scheduler.nextLinearBlockIndex = 0;
  }
  advanceLinearScanCursor();

  // Global SWC geometry index for "already traced?" checks.
  auto swcIndex = std::make_shared<ZSwcSpatialIndex>();
  swcIndex->setZToXYRatio(state.manifest.zToXYRatio);
  swcIndex->rebuild(state.swc);

  const bool useGeometryTraceMask =
    FLAGS_atlas_trace_use_swc_geometry_mask && FLAGS_atlas_autotrace_use_swc_geometry_mask;
  std::shared_ptr<ZSwcSpatialIndex> traceMaskIndex;
  if (useGeometryTraceMask) {
    traceMaskIndex = std::make_shared<ZSwcSpatialIndex>();
    traceMaskIndex->setZToXYRatio(state.manifest.zToXYRatio);
    traceMaskIndex->rebuild(state.swc);
  }

  LOG(INFO) << fmt::format("Base dataset: {}x{}x{}", baseW, baseH, baseD);
  LOG(INFO) << fmt::format("Tracing dataset: {}x{}x{}", grid.shape.width, grid.shape.height, grid.shape.depth);
  LOG(INFO) << fmt::format("Block core: {}x{}x{}, halo={}",
                           grid.block.coreX,
                           grid.block.coreY,
                           grid.block.coreZ,
                           grid.block.halo);
  LOG(INFO) << fmt::format("Threshold mode: {}", state.manifest.thresholdMode);
  if (state.manifest.thresholdMode == "fixed") {
    LOG(INFO) << fmt::format("Subtract constant (fixed): {}", state.manifest.subtractConstant);
  }
  LOG(INFO) << fmt::format("Resume from commit: {}", state.commitId);
  LOG(INFO) << fmt::format("Resume SWC nodes: {}", state.swc.size());
  LOG(INFO) << fmt::format("Resume pending tasks: {}", state.frontier.size());
  LOG(INFO) << fmt::format("Seed-scanned blocks: {}/{}", seedScanned.size(), totalBlocks);

  auto commitCounter = session.latestCommittedIdOrZero();
  CHECK(commitCounter >= state.commitId);

  auto reportProgressFromSeedScan = [&]() {
    if (totalBlocks == 0) {
      this->reportProgress(1.0);
      return;
    }
    const double p = std::min<double>(1.0, static_cast<double>(seedScanned.size()) / static_cast<double>(totalBlocks));
    this->reportProgress(p);
  };

  reportProgressFromSeedScan();

  struct NextBlockPick
  {
    ZBlockedAutoTraceBlockId id{};
    size_t pendingTasksForBlock = 0;
    bool pickedFromFrontier = false;
  };

  auto computeIndexRange =
    [](double coord, int64_t core, int64_t halo, int64_t nblocks) -> std::pair<int64_t, int64_t> {
    CHECK(core > 0);
    CHECK(nblocks > 0);
    const double c = coord;
    const double coreD = static_cast<double>(core);
    const double haloD = static_cast<double>(halo);

    const double bMinD = std::ceil((c - (coreD + haloD - 1.0)) / coreD);
    const double bMaxD = std::floor((c + haloD) / coreD);
    int64_t bMin = static_cast<int64_t>(bMinD);
    int64_t bMax = static_cast<int64_t>(bMaxD);
    if (bMin < 0) {
      bMin = 0;
    }
    if (bMin >= nblocks) {
      bMin = nblocks - 1;
    }
    if (bMax < 0) {
      bMax = 0;
    }
    if (bMax >= nblocks) {
      bMax = nblocks - 1;
    }
    if (bMax < bMin) {
      bMax = bMin;
    }
    return {bMin, bMax};
  };

  auto continuationLeavesImage = [&](const LocalNeuroseg& endLocseg, TraceDirection direction) {
    const ContinuationHint hint = continuationHintForTask(endLocseg, direction, state.manifest.zToXYRatio);
    return continuationWouldLeaveImageForVoxelSampling(grid.shape.width,
                                                       grid.shape.height,
                                                       grid.shape.depth,
                                                       hint.anchor,
                                                       hint.axisDir,
                                                       hint.step,
                                                       hint.radiusXY,
                                                       hint.radiusZ);
  };

  auto findUnvisitedBlockForTask =
    [&](const LocalNeuroseg& endLocseg,
        TraceDirection direction,
        std::optional<uint64_t> forbiddenLinearKey) -> std::optional<ZBlockedAutoTraceBlockId> {
    const ContinuationHint hint = continuationHintForTask(endLocseg, direction, state.manifest.zToXYRatio);
    const int64_t maxCore = std::max({grid.block.coreX, grid.block.coreY, grid.block.coreZ});
    const double radiusBridgeStep = std::max({1.0, hint.radiusXY * 2.0 + 1.0, hint.radiusZ * 2.0 + 1.0});
    const double blockBridgeStep = static_cast<double>(std::max<int64_t>(1, maxCore + grid.block.halo + 1));
    const std::array<std::array<double, 3>, 4> searchPoints = {
      hint.anchor,
      continuationProbePoint(hint),
      continuationPointAtDistance(hint, std::max(hint.step, radiusBridgeStep)),
      continuationPointAtDistance(hint, std::max({hint.step, radiusBridgeStep, blockBridgeStep})),
    };

    const ZBlockedAutoTraceBlockId preferred =
      grid.suggestedBlockForTask(endLocseg, direction, state.manifest.zToXYRatio);
    const uint64_t preferredKey = grid.linearIndexOrThrow(preferred);
    if (!seedScanned.contains(preferredKey) && (!forbiddenLinearKey || preferredKey != *forbiddenLinearKey)) {
      const BlockGrid::Bounds preferredRoi = grid.roiBounds(grid.coreBounds(preferred));
      for (const auto& p : searchPoints) {
        if (pointInBoundsForVoxelSampling(preferredRoi, p)) {
          return preferred;
        }
      }
    }

    const int64_t nx = grid.numBlocksX();
    const int64_t ny = grid.numBlocksY();
    const int64_t nz = grid.numBlocksZ();
    CHECK(nx > 0 && ny > 0 && nz > 0);

    for (const auto& p : searchPoints) {
      const auto [bx0, bx1] = computeIndexRange(p[0], grid.block.coreX, grid.block.halo, nx);
      const auto [by0, by1] = computeIndexRange(p[1], grid.block.coreY, grid.block.halo, ny);
      const auto [bz0, bz1] = computeIndexRange(p[2], grid.block.coreZ, grid.block.halo, nz);

      bool have = false;
      uint64_t bestKey = 0;
      int64_t bestDist = 0;

      for (int64_t bz = bz0; bz <= bz1; ++bz) {
        for (int64_t by = by0; by <= by1; ++by) {
          for (int64_t bx = bx0; bx <= bx1; ++bx) {
            const ZBlockedAutoTraceBlockId cand{.bx = bx, .by = by, .bz = bz};
            const uint64_t key = grid.linearIndexOrThrow(cand);
            if (seedScanned.contains(key) || (forbiddenLinearKey && key == *forbiddenLinearKey)) {
              continue;
            }

            const BlockGrid::Bounds roi = grid.roiBounds(grid.coreBounds(cand));
            if (!pointInBoundsForVoxelSampling(roi, p)) {
              continue;
            }

            const int64_t dist =
              std::abs(bx - preferred.bx) + std::abs(by - preferred.by) + std::abs(bz - preferred.bz);
            if (!have || dist < bestDist || (dist == bestDist && key < bestKey)) {
              have = true;
              bestDist = dist;
              bestKey = key;
            }
          }
        }
      }

      if (have) {
        return grid.blockIdFromLinearOrThrow(bestKey);
      }
    }

    return std::nullopt;
  };

  auto sanitizeFrontierOrThrow = [&]() {
    if (state.frontier.empty()) {
      return;
    }

    std::vector<ZBlockedAutoTracePendingTask> kept;
    kept.reserve(state.frontier.size());
    size_t dropped = 0;
    size_t reassigned = 0;
    size_t terminatedNoHandoff = 0;

    for (auto& t : state.frontier) {
      const std::array<double, 3> p = BlockGrid::taskAnchorPoint(t.endLocseg, t.direction, state.manifest.zToXYRatio);
      const uint64_t key = grid.linearIndexOrThrow(t.suggestedBlock);
      if (!seedScanned.contains(key)) {
        kept.push_back(std::move(t));
        continue;
      }

      // If a task points to a visited block, we either:
      // - drop it if it's already satisfied by existing SWC geometry, or
      // - reassign it to another unvisited block whose ROI still contains the anchor point.
      if (swcIndex->containsPoint(p[0], p[1], p[2])) {
        ++dropped;
        continue;
      }

      if (continuationLeavesImage(t.endLocseg, t.direction)) {
        ++dropped;
        continue;
      }

      if (const auto reassignedBlock = findUnvisitedBlockForTask(t.endLocseg, t.direction, std::nullopt);
          reassignedBlock.has_value()) {
        t.suggestedBlock = *reassignedBlock;
        ++reassigned;
        kept.push_back(std::move(t));
      } else {
        ++terminatedNoHandoff;
      }
    }

    if (dropped > 0 || reassigned > 0 || terminatedNoHandoff > 0) {
      LOG(WARNING) << fmt::format(
        "Blocked auto trace: sanitized frontier after resume (droppedSatisfied={}, reassignedToUnvisited={}, terminatedWithoutHandoff={}).",
        dropped,
        reassigned,
        terminatedNoHandoff);
    }

    state.frontier = std::move(kept);
  };

  sanitizeFrontierOrThrow();

  auto pickNextBlockOrThrow = [&]() -> std::optional<NextBlockPick> {
    // 1) Prefer blocks with the most pending tasks (but only unvisited blocks).
    if (!state.frontier.empty()) {
      std::unordered_map<uint64_t, size_t> counts;
      counts.reserve(state.frontier.size());
      for (const auto& t : state.frontier) {
        CHECK(grid.containsBlockId(t.suggestedBlock))
          << fmt::format("Blocked auto trace invariant violated: frontier task {} has invalid block id ({},{},{}).",
                         t.taskId,
                         t.suggestedBlock.bx,
                         t.suggestedBlock.by,
                         t.suggestedBlock.bz);
        const uint64_t key = grid.linearIndexOrThrow(t.suggestedBlock);
        if (seedScanned.contains(key)) {
          continue; // never revisit a block
        }
        ++counts[key];
      }

      uint64_t bestKey = 0;
      size_t bestCount = 0;
      bool have = false;
      for (const auto& [k, c] : counts) {
        if (!have || c > bestCount || (c == bestCount && k < bestKey)) {
          have = true;
          bestKey = k;
          bestCount = c;
        }
      }

      if (have) {
        NextBlockPick pick;
        pick.id = grid.blockIdFromLinearOrThrow(bestKey);
        pick.pendingTasksForBlock = bestCount;
        pick.pickedFromFrontier = true;
        return pick;
      }
    }

    // 2) Otherwise, pick the next never-visited block in linear order.
    advanceLinearScanCursor();
    const uint64_t cur = state.scheduler.nextLinearBlockIndex;
    if (cur < totalBlocks) {
      NextBlockPick pick;
      pick.id = grid.blockIdFromLinearOrThrow(cur);
      pick.pendingTasksForBlock = 0;
      pick.pickedFromFrontier = false;
      return pick;
    }

    return std::nullopt;
  };

  auto tryAppendContinuationTask = [&](const LocalNeuroseg& endGlobal,
                                       int64_t attachSwcNodeId,
                                       TraceDirection direction,
                                       std::optional<uint64_t> forbiddenLinearKey,
                                       std::string_view reason,
                                       std::vector<ZBlockedAutoTracePendingTask>& out) {
    if (continuationLeavesImage(endGlobal, direction)) {
      return false;
    }

    const auto nextBlock = findUnvisitedBlockForTask(endGlobal, direction, forbiddenLinearKey);
    if (!nextBlock.has_value()) {
      const ContinuationHint hint = continuationHintForTask(endGlobal, direction, state.manifest.zToXYRatio);
      LOG(WARNING) << fmt::format("Blocked auto trace: terminating continuation without handoff block.\n"
                                  "  anchor=({:.3f},{:.3f},{:.3f})\n"
                                  "  direction={}\n"
                                  "  reason={}\n"
                                  "  forbiddenLinearKey={}",
                                  hint.anchor[0],
                                  hint.anchor[1],
                                  hint.anchor[2],
                                  traceDirectionToStringForLog(direction),
                                  std::string(reason),
                                  forbiddenLinearKey ? fmt::format("{}", *forbiddenLinearKey) : std::string("<none>"));
      return false;
    }

    ZBlockedAutoTracePendingTask t;
    t.taskId = nextTaskId++;
    t.attachSwcNodeId = attachSwcNodeId;
    t.direction = direction;
    t.endLocseg = endGlobal;
    t.reason = std::string(reason);
    t.suggestedBlock = *nextBlock;
    out.push_back(std::move(t));
    return true;
  };

  auto tracePendingTaskInBlock = [&](const ZBlockedAutoTracePendingTask& task,
                                     uint64_t forbiddenLinearKey,
                                     const BlockGrid::Bounds& roi,
                                     const ZImg& signal,
                                     std::vector<ZBlockedAutoTraceSwcDeltaNode>& deltaOut,
                                     std::vector<ZBlockedAutoTracePendingTask>& newTasksOut) {
    const double roiMinTraceZ = static_cast<double>(roi.minZ) * state.manifest.zToXYRatio;
    TraceWorkspace tw;
    locsegChainDefaultTraceWorkspaceLegacyLike(tw, signal);
    tw.cancellationToken = m_cancellationToken;
    tw.refit = cfg.refit;
    tw.tuneEnd = cfg.tuneEnd;
    tw.traceMaskUpdating = false;
    tw.resolution = traceResolutionFromZToXYRatioLegacyLike(state.manifest.zToXYRatio);
    prepareTraceScoreThresholdLegacyLike(signal, cfg, TracingModeLegacyLike::Auto, tw);

    std::shared_ptr<ZSwcSpatialIndex> taskSwcMaskIndex = swcIndex;
    if (useGeometryTraceMask) {
      CHECK(traceMaskIndex != nullptr);
      taskSwcMaskIndex = traceMaskIndex;
    }

    tw.traceMaskVolume = std::make_unique<ZSwcGeometryMaskVolume>(
      taskSwcMaskIndex,
      signal.width(),
      signal.height(),
      signal.depth(),
      glm::dvec3{static_cast<double>(roi.minX), static_cast<double>(roi.minY), static_cast<double>(roi.minZ)});

    LocalNeuroseg seedLocseg = task.endLocseg;
    seedLocseg.pos[0] -= static_cast<double>(roi.minX);
    seedLocseg.pos[1] -= static_cast<double>(roi.minY);
    seedLocseg.pos[2] -= roiMinTraceZ;

    LocsegChain chain;
    LocsegNode seedNode;
    seedNode.locseg = seedLocseg;
    traceRecordReset(seedNode.tr);
    traceRecordSetDirection(seedNode.tr, TraceDirection::BothDir);
    (void)chain.addNode(std::move(seedNode), LocsegChainEndLegacyLike::Tail);

    // Disable the "inward" end so we only extend the requested direction.
    if (task.direction == TraceDirection::Forward) {
      traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::HitMark, TraceStatus::Normal);
    } else if (task.direction == TraceDirection::Backward) {
      traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::Normal, TraceStatus::HitMark);
    } else {
      traceWorkspaceSetTraceStatusLegacyLike(tw, TraceStatus::Normal, TraceStatus::Normal);
    }

    maybeCancel(m_cancellationToken);
    traceLocsegLegacyLike(signal, state.manifest.zToXYRatio, chain, tw);
    maybeCancel(m_cancellationToken);

    const auto attachIt = state.nodeById.find(task.attachSwcNodeId);
    if (attachIt == state.nodeById.end()) {
      throw ZException(
        fmt::format("Blocked auto trace: pending task attach node id {} not found in SWC.", task.attachSwcNodeId));
    }

    auto appendNode = [&](int64_t parentId, const LocalNeuroseg& locsegLocal) -> int64_t {
      const int64_t id = nextNodeId++;
      const ZBlockedAutoTraceSwcDeltaNode d =
        makeDeltaNode(id, parentId, locsegLocal, glm::dvec3{roi.minX, roi.minY, roi.minZ}, state.manifest.zToXYRatio);
      appendDeltaNodeToSwcOrThrow(d, state.swc, state.nodeById);
      deltaOut.push_back(d);

      // Incremental geometry index update.
      if (parentId >= 0) {
        auto pit = state.nodeById.find(parentId);
        CHECK(pit != state.nodeById.end());
        const glm::dvec3 a{pit->second->x, pit->second->y, pit->second->z};
        const glm::dvec3 b{d.x, d.y, d.z};
        const double ra = std::max(0.0, pit->second->radius);
        const double rb = std::max(0.0, d.radius);
        swcIndex->insertSegment(a, b, ra, rb);
        if (traceMaskIndex) {
          traceMaskIndex->insertSegment(a, b, ra, rb);
        }
      } else {
        const glm::dvec3 a{d.x, d.y, d.z};
        const glm::dvec3 b{d.x, d.y, d.z};
        const double r = std::max(0.0, d.radius);
        swcIndex->insertSegment(a, b, r, r);
        if (traceMaskIndex) {
          traceMaskIndex->insertSegment(a, b, r, r);
        }
      }
      return id;
    };

    if (chain.length() < 2) {
      if (task.direction == TraceDirection::Forward && tw.traceStatus[1] == TraceStatus::OutOfBound) {
        LocalNeuroseg endGlobal = chain.tail()->locseg;
        endGlobal.pos[0] += static_cast<double>(roi.minX);
        endGlobal.pos[1] += static_cast<double>(roi.minY);
        endGlobal.pos[2] += roiMinTraceZ;
        (void)tryAppendContinuationTask(endGlobal,
                                        task.attachSwcNodeId,
                                        TraceDirection::Forward,
                                        forbiddenLinearKey,
                                        "OutOfBlockHaloNoProgress",
                                        newTasksOut);
      } else if (task.direction == TraceDirection::Backward && tw.traceStatus[0] == TraceStatus::OutOfBound) {
        LocalNeuroseg endGlobal = chain.head()->locseg;
        endGlobal.pos[0] += static_cast<double>(roi.minX);
        endGlobal.pos[1] += static_cast<double>(roi.minY);
        endGlobal.pos[2] += roiMinTraceZ;
        (void)tryAppendContinuationTask(endGlobal,
                                        task.attachSwcNodeId,
                                        TraceDirection::Backward,
                                        forbiddenLinearKey,
                                        "OutOfBlockHaloNoProgress",
                                        newTasksOut);
      }
      return;
    }

    // Attach outward extension; skip the seed node to avoid duplicating the attachment point.
    if (task.direction == TraceDirection::Forward) {
      // Seed is at head. Append nodes head+1 .. tail.
      int64_t parentId = task.attachSwcNodeId;
      bool skippedSeed = false;
      for (const auto& node : chain) {
        if (!skippedSeed) {
          skippedSeed = true;
          continue;
        }
        parentId = appendNode(parentId, node.locseg);
      }

      // Pending task if we ran out of ROI in forward direction.
      if (tw.traceStatus[1] == TraceStatus::OutOfBound) {
        const LocalNeuroseg endLocal = chain.tail()->locseg;
        LocalNeuroseg endGlobal = endLocal;
        endGlobal.pos[0] += static_cast<double>(roi.minX);
        endGlobal.pos[1] += static_cast<double>(roi.minY);
        endGlobal.pos[2] += roiMinTraceZ;
        (void)tryAppendContinuationTask(endGlobal,
                                        deltaOut.back().id,
                                        TraceDirection::Forward,
                                        forbiddenLinearKey,
                                        "OutOfBlockHalo",
                                        newTasksOut);
      }
      return;
    }

    if (task.direction == TraceDirection::Backward) {
      // Seed is at tail. Append nodes tail-1 .. head, reversed.
      std::vector<const LocsegNode*> nodes;
      nodes.reserve(static_cast<size_t>(chain.length()));
      for (const auto& n : chain) {
        nodes.push_back(&n);
      }
      int64_t parentId = task.attachSwcNodeId;
      // Skip tail (seed).
      for (int i = static_cast<int>(nodes.size()) - 2; i >= 0; --i) {
        parentId = appendNode(parentId, nodes[static_cast<size_t>(i)]->locseg);
      }

      if (tw.traceStatus[0] == TraceStatus::OutOfBound) {
        const LocalNeuroseg endLocal = chain.head()->locseg;
        LocalNeuroseg endGlobal = endLocal;
        endGlobal.pos[0] += static_cast<double>(roi.minX);
        endGlobal.pos[1] += static_cast<double>(roi.minY);
        endGlobal.pos[2] += roiMinTraceZ;
        (void)tryAppendContinuationTask(endGlobal,
                                        deltaOut.back().id,
                                        TraceDirection::Backward,
                                        forbiddenLinearKey,
                                        "OutOfBlockHalo",
                                        newTasksOut);
      }
    }
  };

  // Main loop: one commit per processed block.
  while (true) {
    maybeCancel(m_cancellationToken);

    const auto nextPickOpt = pickNextBlockOrThrow();
    if (!nextPickOpt.has_value()) {
      break;
    }
    const ZBlockedAutoTraceBlockId blockId = nextPickOpt->id;
    const size_t pendingTasksForThisBlock = nextPickOpt->pendingTasksForBlock;
    const bool pickedFromFrontier = nextPickOpt->pickedFromFrontier;

    const uint64_t blockKey = grid.linearIndexOrThrow(blockId);
    CHECK(!seedScanned.contains(blockKey)) << "Blocked auto trace invariant violated: revisiting a block is forbidden.";

    const BlockGrid::Bounds core = grid.coreBounds(blockId);
    const BlockGrid::Bounds roi = grid.roiBounds(core);

    const int64_t roiW = roi.maxX - roi.minX;
    const int64_t roiH = roi.maxY - roi.minY;
    const int64_t roiD = roi.maxZ - roi.minZ;
    CHECK(roiW >= 0 && roiH >= 0 && roiD >= 0);

    LOG(INFO) << fmt::format(
      "Blocked auto trace: block ({},{},{}) start (linear={}/{}, pickReason={}, frontierThisBlock={}, frontierTotal={}, visitedBlocks={}/{}, swcNodes={})",
      blockId.bx,
      blockId.by,
      blockId.bz,
      blockKey + 1,
      totalBlocks,
      pickedFromFrontier ? "frontier" : "seedScan",
      pendingTasksForThisBlock,
      state.frontier.size(),
      seedScanned.size(),
      totalBlocks,
      state.swc.size());
    LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) core={}, roi={}, roiSize={}x{}x{}",
                             blockId.bx,
                             blockId.by,
                             blockId.bz,
                             BlockGrid::boundsToString(core),
                             BlockGrid::boundsToString(roi),
                             roiW,
                             roiH,
                             roiD);

    LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) load ROI signal ...",
                             blockId.bx,
                             blockId.by,
                             blockId.bz);
    RoiSignalResult roiResult = m_roiProvider(roi.minX, roi.minY, roi.minZ, roiW, roiH, roiD, m_cancellationToken);
    maybeCancel(m_cancellationToken);
    LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) load ROI signal done.",
                             blockId.bx,
                             blockId.by,
                             blockId.bz);

    std::shared_ptr<ZImg> sharedSignal;
    std::unique_ptr<ZImg> ownedSignal;
    ZImg* signalPtr = nullptr;
    if (roiResult.status == RoiSignalResult::Status::Ok) {
      CHECK(roiResult.signal != nullptr);
      // Blocked auto trace bypasses the assembled region cache, so it can preprocess the ROI in place.
      sharedSignal = std::move(roiResult.signal);
      signalPtr = sharedSignal.get();
      CHECK(signalPtr->numChannels() == 1);
      CHECK(signalPtr->numTimes() == 1);
      CHECK(static_cast<int64_t>(signalPtr->width()) == roiW);
      CHECK(static_cast<int64_t>(signalPtr->height()) == roiH);
      CHECK(static_cast<int64_t>(signalPtr->depth()) == roiD);
    } else {
      CHECK(roiResult.status == RoiSignalResult::Status::AllZero);
      ZImgInfo info(static_cast<size_t>(std::max<int64_t>(roiW, 0)),
                    static_cast<size_t>(std::max<int64_t>(roiH, 0)),
                    static_cast<size_t>(std::max<int64_t>(roiD, 0)),
                    1,
                    1,
                    m_signalInfo.bytesPerVoxel,
                    m_signalInfo.voxelFormat);
      ownedSignal = std::make_unique<ZImg>(info);
      ownedSignal->fill(0);
      signalPtr = ownedSignal.get();
    }
    CHECK(signalPtr != nullptr);
    ZImg& signal = *signalPtr;

    // Preprocess the ROI signal (block-local, deterministic).
    if (!signal.isEmpty()) {
      if (state.manifest.thresholdMode == "auto") {
        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) preprocess (subtract background) ...",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz);
        const int commonIntensity = subtractBackgroundLegacyLike(signal, /*minFr*/ 0.5, /*maxIter*/ 3);
        VLOG(1) << fmt::format("Blocked auto trace: block ({},{},{}) subtract background done (commonIntensity={}).",
                               blockId.bx,
                               blockId.by,
                               blockId.bz,
                               commonIntensity);
      } else if (state.manifest.thresholdMode == "fixed") {
        if (state.manifest.subtractConstant != 0.0) {
          VLOG(1) << fmt::format("Blocked auto trace: block ({},{},{}) subtract constant (fixed={}).",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz,
                                 state.manifest.subtractConstant);
          signal -= state.manifest.subtractConstant;
        }
      } else {
        CHECK(false) << fmt::format("Blocked auto trace: unexpected thresholdMode '{}'.", state.manifest.thresholdMode);
      }
    }

    // Collect per-commit delta nodes.
    std::vector<ZBlockedAutoTraceSwcDeltaNode> deltaNodes;
    deltaNodes.reserve(4096);

    // Trace pending tasks assigned to this block.
    std::vector<ZBlockedAutoTracePendingTask> keptTasks;
    keptTasks.reserve(state.frontier.size());
    std::vector<ZBlockedAutoTracePendingTask> newTasks;
    newTasks.reserve(16);

    const size_t frontierBeforeTasks = state.frontier.size();
    size_t pendingTasksTraced = 0;
    for (const auto& task : state.frontier) {
      const ZBlockedAutoTraceBlockId sid = task.suggestedBlock;
      if (sid.bx == blockId.bx && sid.by == blockId.by && sid.bz == blockId.bz) {
        ++pendingTasksTraced;
        tracePendingTaskInBlock(task, blockKey, roi, signal, deltaNodes, newTasks);
      } else {
        keptTasks.push_back(task);
      }
    }

    // After tracing tasks, frontier becomes kept + new.
    state.frontier = std::move(keptTasks);
    for (auto& nt : newTasks) {
      state.frontier.push_back(std::move(nt));
    }

    // Seed scan only once per block.
    const size_t deltaNodesAfterPending = deltaNodes.size();
    const size_t frontierAfterPending = state.frontier.size();

    if (pendingTasksTraced > 0) {
      LOG(INFO) << fmt::format(
        "Blocked auto trace: block ({},{},{}) traced pending tasks (assigned={}, frontier {} -> {}, deltaNodesFromPending={})",
        blockId.bx,
        blockId.by,
        blockId.bz,
        pendingTasksTraced,
        frontierBeforeTasks,
        frontierAfterPending,
        deltaNodesAfterPending);
    }

    if (!signal.isEmpty()) {
      MakeMaskDiagnosticsLegacyLike maskDiag;
      LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) make mask ...",
                               blockId.bx,
                               blockId.by,
                               blockId.bz);
      std::optional<ZImg> maskOpt = makeMaskLegacyLike(signal, cfg, &maskDiag);
      maybeCancel(m_cancellationToken);

      if (maskOpt.has_value()) {
        ZImg mask = std::move(*maskOpt);
        VLOG(1) << fmt::format("Blocked auto trace: block ({},{},{}) mask threshold={}",
                               blockId.bx,
                               blockId.by,
                               blockId.bz,
                               maskDiag.binarizeThreshold);

        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) extract seeds ...",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz);
        Geo3dScalarField seeds = extractSeedOriginalLegacyLike(mask, m_cancellationToken);
        maybeCancel(m_cancellationToken);
        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) extracted {} seeds.",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz,
                                 seeds.size());

        RemoveNoisySeedDiagnosticsLegacyLike seedDiag;
        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) remove noisy seeds ...",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz);
        seeds = removeNoisySeedLegacyLike(std::move(seeds),
                                          mask,
                                          cfg.seedMethod,
                                          /*screeningSeed*/ true,
                                          m_cancellationToken,
                                          &seedDiag);
        maybeCancel(m_cancellationToken);
        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) {} seeds after noise removal.",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz,
                                 seeds.size());

        size_t seedsSkippedHalo = 0;
        Geo3dScalarField keptSeeds =
          filterSeedsToCoreLegacyLike(std::move(seeds), roi, core, m_cancellationToken, &seedsSkippedHalo);
        CHECK(keptSeeds.points.size() == keptSeeds.values.size());

        if (keptSeeds.points.empty()) {
          LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) no seeds to trace after halo filter.",
                                   blockId.bx,
                                   blockId.by,
                                   blockId.bz);
        } else {
          const double roiMinTraceZ = static_cast<double>(roi.minZ) * state.manifest.zToXYRatio;

          TraceWorkspace tw;
          locsegChainDefaultTraceWorkspaceLegacyLike(tw, signal);
          tw.cancellationToken = m_cancellationToken;
          tw.refit = cfg.refit;
          tw.tuneEnd = cfg.tuneEnd;
          tw.traceMaskUpdating = true;
          tw.resolution = traceResolutionFromZToXYRatioLegacyLike(state.manifest.zToXYRatio);

          if (useGeometryTraceMask) {
            CHECK(traceMaskIndex != nullptr);
            tw.traceMaskVolume = std::make_unique<ZSwcGeometryMaskVolume>(
              traceMaskIndex,
              signal.width(),
              signal.height(),
              signal.depth(),
              glm::dvec3{static_cast<double>(roi.minX), static_cast<double>(roi.minY), static_cast<double>(roi.minZ)});
          } else {
            // Dense mode: prefill the ROI mask from already-traced global SWC so this block is processed like an
            // in-memory run on an ROI with pre-existing traced geometry.
            traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ true);
            CHECK(tw.traceMask != nullptr);
            labelSwcIndexIntoRoiDenseTraceMaskForRecoveryLegacyLike(*swcIndex,
                                                                    *tw.traceMask,
                                                                    roi,
                                                                    state.manifest.zToXYRatio);
          }

          const size_t seedsBeforeTracedFilter = keptSeeds.size();
          keptSeeds = removeTracedSeedLegacyLike(keptSeeds, tw);
          const size_t seedsSkippedTracedMask = seedsBeforeTracedFilter - keptSeeds.size();

          LOG(INFO) << fmt::format(
            "Blocked auto trace: block ({},{},{}) kept {} seeds after halo/trace-mask filter (skippedHalo={}, skippedTracedMask={}).",
            blockId.bx,
            blockId.by,
            blockId.bz,
            keptSeeds.size(),
            seedsSkippedHalo,
            seedsSkippedTracedMask);

          if (keptSeeds.points.empty()) {
            LOG(INFO) << fmt::format(
              "Blocked auto trace: block ({},{},{}) no seeds to trace after halo/trace-mask filter.",
              blockId.bx,
              blockId.by,
              blockId.bz);
            continue;
          }

          LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) sort seeds (in-memory pipeline) ...",
                                   blockId.bx,
                                   blockId.by,
                                   blockId.bz);
          prepareTraceScoreThresholdLegacyLike(signal, cfg, TracingModeLegacyLike::Seed, tw);
          SeedSortResultLegacyLike sorted = sortSeedsLegacyLike(keptSeeds, signal, state.manifest.zToXYRatio, tw);
          std::optional<ZImg> baseMask = std::move(sorted.baseMask);

          LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) trace all seeds (in-memory pipeline) ...",
                                   blockId.bx,
                                   blockId.by,
                                   blockId.bz);
          prepareTraceScoreThresholdLegacyLike(signal, cfg, TracingModeLegacyLike::Auto, tw);

          std::vector<TraceAllSeedsEndStatusLegacyLike> chainEnds;
          std::vector<std::unique_ptr<LocsegChain>> chains = traceAllSeedsLegacyLike(signal,
                                                                                     state.manifest.zToXYRatio,
                                                                                     sorted.locsegArray,
                                                                                     sorted.scoreArray,
                                                                                     tw,
                                                                                     &chainEnds);
          maybeCancel(m_cancellationToken);
          CHECK(chainEnds.size() == chains.size());

          if (cfg.recover > 0) {
            if (useGeometryTraceMask && !tw.traceMask && tw.traceMaskVolume) {
              LOG(INFO) << fmt::format(
                "Blocked auto trace: block ({},{},{}) materialize dense trace mask for legacy recovery ...",
                blockId.bx,
                blockId.by,
                blockId.bz);

              traceWorkspaceInitTraceMaskLegacyLike(tw, signal, /*clearing*/ true);
              CHECK(tw.traceMask != nullptr);
              labelSwcIndexIntoRoiDenseTraceMaskForRecoveryLegacyLike(*swcIndex,
                                                                      *tw.traceMask,
                                                                      roi,
                                                                      state.manifest.zToXYRatio);

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
                                            state.manifest.zToXYRatio,
                                            /*begin*/ 0,
                                            /*end*/ chain->length() - 1,
                                            labelWs);
              }
            }

            LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) recover (in-memory pipeline) ...",
                                     blockId.bx,
                                     blockId.by,
                                     blockId.bz);
            RecoverResultLegacyLike recovered =
              recoverLegacyLike(signal,
                                cfg,
                                state.manifest.zToXYRatio,
                                mask,
                                std::move(baseMask),
                                tw,
                                [&](Geo3dScalarField recoveredSeeds, ZImg& leftoverMask) {
                                  return postProcessBlockedRecoverySeedsLegacyLike(std::move(recoveredSeeds),
                                                                                   leftoverMask,
                                                                                   cfg.seedMethod,
                                                                                   roi,
                                                                                   core,
                                                                                   m_cancellationToken);
                                });
            maybeCancel(m_cancellationToken);

            CHECK(recovered.chains.size() == recovered.chainEndStatuses.size());
            for (auto& c : recovered.chains) {
              chains.push_back(std::move(c));
            }
            for (const auto& e : recovered.chainEndStatuses) {
              chainEnds.push_back(e);
            }
            CHECK(chainEnds.size() == chains.size());

            LOG(INFO) << fmt::format(
              "Blocked auto trace: block ({},{},{}) recover done (chainsFromRecover={}, totalChains={}).",
              blockId.bx,
              blockId.by,
              blockId.bz,
              recovered.chainEndStatuses.size(),
              chains.size());
          }

          LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) append traced chains to SWC (chains={}) ...",
                                   blockId.bx,
                                   blockId.by,
                                   blockId.bz,
                                   chains.size());

          size_t chainsAppended = 0;
          size_t frontierTasksAdded = 0;

          for (size_t ci = 0; ci < chains.size(); ++ci) {
            if (!chains[ci] || chains[ci]->empty()) {
              continue;
            }

            const TraceAllSeedsEndStatusLegacyLike endStatus = chainEnds[ci];
            const LocsegChain& chain = *chains[ci];

            // Append as a new root chain, then immediately try to attach that branch
            // to the existing global SWC in ROI-aware image space.
            const std::array<double, 3> signalOrigin = {static_cast<double>(roi.minX),
                                                        static_cast<double>(roi.minY),
                                                        static_cast<double>(roi.minZ)};
            int64_t parentId = -1;
            int64_t firstId = -1;
            int64_t lastId = -1;
            std::vector<int64_t> newNodeIds;
            newNodeIds.reserve(static_cast<size_t>(chain.length()));
            int idx = 0;
            for (const auto& cn : chain) {
              const int64_t id = nextNodeId++;
              const ZBlockedAutoTraceSwcDeltaNode d = makeDeltaNode(id,
                                                                    parentId,
                                                                    cn.locseg,
                                                                    glm::dvec3{roi.minX, roi.minY, roi.minZ},
                                                                    state.manifest.zToXYRatio);
              appendDeltaNodeToSwcOrThrow(d, state.swc, state.nodeById);
              newNodeIds.push_back(id);

              if (idx == 0) {
                firstId = id;
              }
              lastId = id;

              parentId = id;
              ++idx;
            }

            ConnectBranchToHostResultLegacyLike connResult;
            std::vector<ZSwc::SwcTreeNode> hostRoots;
            if (firstId > 0) {
              auto branchRootIt = state.nodeById.find(firstId);
              CHECK(branchRootIt != state.nodeById.end());
              const ZSwc::SwcTreeNode branchRoot = branchRootIt->second;
              hostRoots.reserve(state.swc.numRoots());
              for (auto it = state.swc.beginRoot(); it != state.swc.endRoot(); ++it) {
                if (it != branchRoot) {
                  hostRoots.emplace_back(it);
                }
              }

              connectBranchToHostLegacyLike(state.swc, hostRoots, branchRoot, signal, signalOrigin, &connResult);
              if (connResult.removedNodeId > 0) {
                state.nodeById.erase(connResult.removedNodeId);
              }
            }

            for (const int64_t id : newNodeIds) {
              auto nit = state.nodeById.find(id);
              if (nit == state.nodeById.end()) {
                continue;
              }
              const ZBlockedAutoTraceSwcDeltaNode d = makeDeltaNodeFromSwcNode(nit->second);
              deltaNodes.push_back(d);

              if (d.parentId >= 0) {
                auto pit = state.nodeById.find(d.parentId);
                CHECK(pit != state.nodeById.end());
                const glm::dvec3 a{pit->second->x, pit->second->y, pit->second->z};
                const glm::dvec3 b{d.x, d.y, d.z};
                const double ra = std::max(0.0, pit->second->radius);
                const double rb = std::max(0.0, d.radius);
                swcIndex->insertSegment(a, b, ra, rb);
                if (traceMaskIndex) {
                  traceMaskIndex->insertSegment(a, b, ra, rb);
                }
              } else {
                const glm::dvec3 a{d.x, d.y, d.z};
                const glm::dvec3 b{d.x, d.y, d.z};
                const double r = std::max(0.0, d.radius);
                swcIndex->insertSegment(a, b, r, r);
                if (traceMaskIndex) {
                  traceMaskIndex->insertSegment(a, b, r, r);
                }
              }
            }

            ++chainsAppended;

            // Emit frontier tasks if this chain reached the ROI boundary.
            const bool headConnected = connResult.connected && !connResult.hookWasTail;
            const bool tailConnected = connResult.connected && connResult.hookWasTail;

            if (endStatus[0] == TraceStatus::OutOfBound && !headConnected && firstId > 0 &&
                state.nodeById.contains(firstId)) {
              LocalNeuroseg endGlobal = chain.head()->locseg;
              endGlobal.pos[0] += static_cast<double>(roi.minX);
              endGlobal.pos[1] += static_cast<double>(roi.minY);
              endGlobal.pos[2] += roiMinTraceZ;
              if (tryAppendContinuationTask(endGlobal,
                                            firstId,
                                            TraceDirection::Backward,
                                            blockKey,
                                            "OutOfBlockHalo",
                                            state.frontier)) {
                ++frontierTasksAdded;
              }
            }
            if (endStatus[1] == TraceStatus::OutOfBound && !tailConnected && lastId > 0 &&
                state.nodeById.contains(lastId)) {
              LocalNeuroseg endGlobal = chain.tail()->locseg;
              endGlobal.pos[0] += static_cast<double>(roi.minX);
              endGlobal.pos[1] += static_cast<double>(roi.minY);
              endGlobal.pos[2] += roiMinTraceZ;
              if (tryAppendContinuationTask(endGlobal,
                                            lastId,
                                            TraceDirection::Forward,
                                            blockKey,
                                            "OutOfBlockHalo",
                                            state.frontier)) {
                ++frontierTasksAdded;
              }
            }
          }

          const size_t deltaNodesAfterSeeds = deltaNodes.size();
          const size_t deltaNodesFromSeeds = deltaNodesAfterSeeds - deltaNodesAfterPending;
          LOG(INFO) << fmt::format(
            "Blocked auto trace: block ({},{},{}) traced seeds done (chainsAppended={}, deltaNodesFromSeeds={}, frontierTasksAdded={}, frontierNow={})",
            blockId.bx,
            blockId.by,
            blockId.bz,
            chainsAppended,
            deltaNodesFromSeeds,
            frontierTasksAdded,
            state.frontier.size());
        }
      } else {
        LOG(INFO) << fmt::format("Blocked auto trace: block ({},{},{}) make mask failed (null mask).",
                                 blockId.bx,
                                 blockId.by,
                                 blockId.bz);
      }
    }

    seedScanned.insert(blockKey);
    advanceLinearScanCursor();
    // Under the "visit each block once" policy, no frontier task may point to a visited block (including the
    // current one) after we commit this block. Such a task would require revisiting, which is forbidden.
    for (const auto& t : state.frontier) {
      CHECK(grid.containsBlockId(t.suggestedBlock))
        << fmt::format("Blocked auto trace invariant violated: frontier task {} has invalid block id ({},{},{}).",
                       t.taskId,
                       t.suggestedBlock.bx,
                       t.suggestedBlock.by,
                       t.suggestedBlock.bz);
      const uint64_t k = grid.linearIndexOrThrow(t.suggestedBlock);
      CHECK(!seedScanned.contains(k)) << fmt::format(
        "Blocked auto trace invariant violated: frontier task {} targets visited block ({},{},{}).",
        t.taskId,
        t.suggestedBlock.bx,
        t.suggestedBlock.by,
        t.suggestedBlock.bz);
    }

    // Commit checkpoint.
    {
      std::vector<uint64_t> seedScannedKeys(seedScanned.begin(), seedScanned.end());
      std::sort(seedScannedKeys.begin(), seedScannedKeys.end());
      std::vector<ZBlockedAutoTraceBlockId> seedScannedSnapshot;
      seedScannedSnapshot.reserve(seedScannedKeys.size());
      for (uint64_t key : seedScannedKeys) {
        seedScannedSnapshot.push_back(grid.blockIdFromLinearOrThrow(key));
      }

      ZBlockedAutoTraceCommitWrite commit;
      commit.info.formatVersion = kBlockedAutoTraceCommitFormatVersion;
      commit.info.commitId = ++commitCounter;
      commit.info.blockId = blockId;
      commit.info.didSeedScan = true;
      commit.info.newSwcNodes = deltaNodes.size();
      commit.info.pendingTasks = state.frontier.size();
      commit.swcDeltaNodes = std::move(deltaNodes);
      commit.frontier = state.frontier;
      commit.scheduler = state.scheduler;

      session.writeCommitOrThrow(commit, state.swc, seedScannedSnapshot);

      // Best-effort rolling SWC artifact (append-only; used for progress inspection and faster resume).
      try {
        session.appendToRollingSwcOrThrow(commit.info.commitId, commit.swcDeltaNodes);
      }
      catch (const std::exception& e) {
        LOG(WARNING) << fmt::format("Blocked auto trace: failed to update result_tracing.swc for commit {}: {}",
                                    commit.info.commitId,
                                    e.what());
      }
      catch (...) {
        LOG(WARNING) << fmt::format("Blocked auto trace: failed to update result_tracing.swc for commit {}.",
                                    commit.info.commitId);
      }

      LOG(INFO) << fmt::format(
        "Blocked auto trace: commit {} written (block=({},{},{}), deltaNodes={}, frontier={}, visitedBlocks={}/{})",
        commit.info.commitId,
        blockId.bx,
        blockId.by,
        blockId.bz,
        commit.info.newSwcNodes,
        commit.info.pendingTasks,
        seedScanned.size(),
        totalBlocks);
    }

    reportProgressFromSeedScan();
  }

  LOG(INFO) << fmt::format("Blocked auto trace: finished (SWC nodes={}, pending tasks={}, visitedBlocks={}/{})",
                           state.swc.size(),
                           state.frontier.size(),
                           seedScanned.size(),
                           totalBlocks);

  CHECK(state.frontier.empty()) << fmt::format(
    "Blocked auto trace invariant violated: finished with {} pending tasks. This would require revisiting blocks.",
    state.frontier.size());

  if (state.swc.empty()) {
    LOG(INFO) << "Blocked auto trace: no SWC generated.";
    return;
  }

  LOG(INFO) << "Blocked auto trace: writing final SWC ...";
  // Write final SWC artifact. Unlike whole-volume auto trace, blocked trace keeps
  // distinct roots as a forest and does not run a final reconnect-to-one-tree pass.
  ZSwc finalTree = state.swc;
  writeFinalSwcAtomicOrThrow(finalTree);
  m_hasResult = true;

  LOG(INFO) << "Blocked auto trace: finished.";
}

void ZNeutubeBlockedAutoTraceProcess::read(const json::object& jo)
{
  if (auto it = jo.find("selected_channel"); it != jo.end()) {
    m_selectedChannel = json::value_to<size_t>(it->value());
  }
  if (auto it = jo.find("selected_time"); it != jo.end()) {
    m_selectedTime = json::value_to<size_t>(it->value());
  }
  if (auto it = jo.find("z_scale"); it != jo.end()) {
    setZToXYRatio(json::value_to<double>(it->value()));
  }
  if (auto it = jo.find("dataset_id"); it != jo.end() && it->value().is_string()) {
    const auto& s = it->value().as_string();
    m_datasetId.assign(s.data(), s.size());
  }
  if (auto it = jo.find("signal_downsample_ratio"); it != jo.end() && it->value().is_array()) {
    const auto& a = it->value().as_array();
    CHECK(a.size() == 3);
    m_signalDownsampleRatio = {
      json::value_to<size_t>(a.at(0)),
      json::value_to<size_t>(a.at(1)),
      json::value_to<size_t>(a.at(2)),
    };
    CHECK(m_signalDownsampleRatio[0] > 0);
    CHECK(m_signalDownsampleRatio[1] > 0);
    CHECK(m_signalDownsampleRatio[2] > 0);
  }
  if (auto it = jo.find("block_core_size"); it != jo.end()) {
    const int64_t v = json::value_to<int64_t>(it->value());
    CHECK(v > 0);
    m_blockCoreX = v;
    m_blockCoreY = v;
    m_blockCoreZ = v;
  }
  if (auto it = jo.find("block_core_x"); it != jo.end()) {
    const int64_t v = json::value_to<int64_t>(it->value());
    CHECK(v > 0);
    m_blockCoreX = v;
  }
  if (auto it = jo.find("block_core_y"); it != jo.end()) {
    const int64_t v = json::value_to<int64_t>(it->value());
    CHECK(v > 0);
    m_blockCoreY = v;
  }
  if (auto it = jo.find("block_core_z"); it != jo.end()) {
    const int64_t v = json::value_to<int64_t>(it->value());
    CHECK(v > 0);
    m_blockCoreZ = v;
  }
  if (auto it = jo.find("block_halo"); it != jo.end()) {
    const int64_t v = json::value_to<int64_t>(it->value());
    CHECK(v >= 0);
    m_blockHalo = v;
  }
  if (auto it = jo.find("trace_config_path"); it != jo.end()) {
    m_traceConfigPath = json::value_to<QString>(it->value());
  }
  if (auto it = jo.find("trace_level"); it != jo.end()) {
    m_traceLevel = json::value_to<int>(it->value());
  }
  if (auto it = jo.find("do_resample_after_tracing"); it != jo.end()) {
    m_doResampleAfterTracing = json::value_to<bool>(it->value());
  }
  if (auto it = jo.find("doc_has_any_swc"); it != jo.end()) {
    m_docHasAnySwc = json::value_to<bool>(it->value());
  }
  if (auto it = jo.find("output_swc_path"); it != jo.end()) {
    m_outputSwcPath = json::value_to<QString>(it->value());
  }
  if (auto it = jo.find("output_session_dir"); it != jo.end()) {
    m_outputSessionDir = json::value_to<QString>(it->value());
  }

  if (auto it = jo.find("algo_overrides"); it != jo.end() && it->value().is_object()) {
    const auto& ao = it->value().as_object();
    TraceConfig cfg;
    if (auto f = ao.find("minAutoScore"); f != ao.end()) {
      cfg.minAutoScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("minManualScore"); f != ao.end()) {
      cfg.minManualScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("minSeedScore"); f != ao.end()) {
      cfg.minSeedScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("min2dScore"); f != ao.end()) {
      cfg.min2dScore = json::value_to<double>(f->value());
    }
    if (auto f = ao.find("refit"); f != ao.end()) {
      cfg.refit = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("spTest"); f != ao.end()) {
      cfg.spTest = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("crossoverTest"); f != ao.end()) {
      cfg.crossoverTest = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("tuneEnd"); f != ao.end()) {
      cfg.tuneEnd = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("edgePath"); f != ao.end()) {
      cfg.edgePath = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("enhanceMask"); f != ao.end()) {
      cfg.enhanceMask = json::value_to<bool>(f->value());
    }
    if (auto f = ao.find("seedMethod"); f != ao.end()) {
      cfg.seedMethod = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("recover"); f != ao.end()) {
      cfg.recover = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("chainScreenCount"); f != ao.end()) {
      cfg.chainScreenCount = json::value_to<int>(f->value());
    }
    if (auto f = ao.find("maxEucDist"); f != ao.end()) {
      cfg.maxEucDist = json::value_to<double>(f->value());
    }
    setAlgoConfigOverrides(cfg);
  } else {
    clearAlgoConfigOverrides();
  }
}

void ZNeutubeBlockedAutoTraceProcess::write(json::object& jo) const
{
  jo["selected_channel"] = json::value_from(m_selectedChannel);
  jo["selected_time"] = json::value_from(m_selectedTime);
  CHECK(m_zToXYRatio.has_value());
  jo["z_scale"] = json::value_from(*m_zToXYRatio);
  jo["dataset_id"] = json::value_from(m_datasetId);
  {
    json::array ratio;
    ratio.push_back(m_signalDownsampleRatio[0]);
    ratio.push_back(m_signalDownsampleRatio[1]);
    ratio.push_back(m_signalDownsampleRatio[2]);
    jo["signal_downsample_ratio"] = std::move(ratio);
  }
  if (m_blockCoreX.has_value() && m_blockCoreY.has_value() && m_blockCoreZ.has_value()) {
    if (*m_blockCoreX == *m_blockCoreY && *m_blockCoreX == *m_blockCoreZ) {
      jo["block_core_size"] = json::value_from(*m_blockCoreX);
    } else {
      jo["block_core_x"] = json::value_from(*m_blockCoreX);
      jo["block_core_y"] = json::value_from(*m_blockCoreY);
      jo["block_core_z"] = json::value_from(*m_blockCoreZ);
    }
  } else {
    if (m_blockCoreX.has_value()) {
      jo["block_core_x"] = json::value_from(*m_blockCoreX);
    }
    if (m_blockCoreY.has_value()) {
      jo["block_core_y"] = json::value_from(*m_blockCoreY);
    }
    if (m_blockCoreZ.has_value()) {
      jo["block_core_z"] = json::value_from(*m_blockCoreZ);
    }
  }
  if (m_blockHalo.has_value()) {
    jo["block_halo"] = json::value_from(*m_blockHalo);
  }
  jo["trace_config_path"] = json::value_from(m_traceConfigPath);
  jo["trace_level"] = json::value_from(m_traceLevel);
  jo["do_resample_after_tracing"] = json::value_from(m_doResampleAfterTracing);
  jo["doc_has_any_swc"] = json::value_from(m_docHasAnySwc);
  jo["output_swc_path"] = json::value_from(m_outputSwcPath);
  jo["output_session_dir"] = json::value_from(m_outputSessionDir);

  if (m_haveAlgoOverrides) {
    json::object ao;
    ao["minAutoScore"] = json::value_from(m_algoOverrides.minAutoScore);
    ao["minManualScore"] = json::value_from(m_algoOverrides.minManualScore);
    ao["minSeedScore"] = json::value_from(m_algoOverrides.minSeedScore);
    ao["min2dScore"] = json::value_from(m_algoOverrides.min2dScore);
    ao["refit"] = json::value_from(m_algoOverrides.refit);
    ao["spTest"] = json::value_from(m_algoOverrides.spTest);
    ao["crossoverTest"] = json::value_from(m_algoOverrides.crossoverTest);
    ao["tuneEnd"] = json::value_from(m_algoOverrides.tuneEnd);
    ao["edgePath"] = json::value_from(m_algoOverrides.edgePath);
    ao["enhanceMask"] = json::value_from(m_algoOverrides.enhanceMask);
    ao["seedMethod"] = json::value_from(m_algoOverrides.seedMethod);
    ao["recover"] = json::value_from(m_algoOverrides.recover);
    ao["chainScreenCount"] = json::value_from(m_algoOverrides.chainScreenCount);
    ao["maxEucDist"] = json::value_from(m_algoOverrides.maxEucDist);
    jo["algo_overrides"] = std::move(ao);
  }
}

} // namespace nim
