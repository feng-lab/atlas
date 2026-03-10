#include "zneutubelocsegchainshortestpath.h"

#include "zneutubegeo3dellipse.h"
#include "zneutubeimgsampling.h"
#include "zneutubeinthistogram.h"
#include "zneutubemathutils.h"
#include "zneutubelocsegchaincircle.h"
#include "zneutubelocsegchainknot.h"
#include "zneutubelocsegchainmetrics.h"
#include "zneutubespgrow.h"
#include "zneutubestackroute.h"
#include "zneutubetracelocseglabel.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace nim {

namespace {

[[nodiscard]] double coordinate3dDistance(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] bool inCloseRange3(const std::array<double, 3>& p, int x0, int x1, int y0, int y1, int z0, int z1)
{
  return (p[0] >= static_cast<double>(x0)) && (p[0] <= static_cast<double>(x1)) && (p[1] >= static_cast<double>(y0)) &&
         (p[1] <= static_cast<double>(y1)) && (p[2] >= static_cast<double>(z0)) && (p[2] <= static_cast<double>(z1));
}

[[nodiscard]] Geo3dEllipseLegacyLike localNeurosegToGeo3dEllipseZLegacyLike(const LocalNeuroseg& locseg, double z)
{
  // Port of tz_local_neuroseg.c::Local_Neuroseg_To_Geo3d_Ellipse_Z().
  Geo3dEllipseLegacyLike out;
  out.radius = neurosegRadiusLegacyLike(locseg.seg, z);
  out.scale = locseg.seg.scale;
  out.center = localNeurosegAxisPositionLegacyLike(locseg, z);
  out.orientation = {locseg.seg.theta, locseg.seg.psi};
  out.alpha = locseg.seg.alpha;
  return out;
}

[[nodiscard]] std::vector<Geo3dEllipseLegacyLike>
locsegChainKnotArrayToEllipseZLegacyLike(const LocsegChainKnotArrayLegacyLike& ka, double zScaleFactor)
{
  // Port of tz_locseg_chain_knot.c::Locseg_Chain_Knot_Array_To_Ellipse_Z().
  CHECK(ka.chain != nullptr);

  const int n = static_cast<int>(ka.knots.size());
  std::vector<Geo3dEllipseLegacyLike> ellipses;
  ellipses.reserve(static_cast<size_t>(n));

  int index = 0;
  int knotIndex = 0;

  for (const auto& node : *ka.chain) {
    LocalNeuroseg locseg2 = node.locseg;
    if (zScaleFactor != 1.0) {
      localNeurosegScaleZLegacyLike(locseg2, zScaleFactor);
    }

    const LocsegChainKnotLegacyLike* knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
    while (knot != nullptr) {
      if (knot->id == index) {
        const double axisZ = knot->offset * (locseg2.seg.h - 1.0);
        ellipses.push_back(localNeurosegToGeo3dEllipseZLegacyLike(locseg2, axisZ));
        ++knotIndex;
        knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
      } else {
        break;
      }
    }

    ++index;
  }

  CHECK(knotIndex == n) << "Not all knots were converted to ellipses. knotIndex=" << knotIndex << " n=" << n;
  return ellipses;
}

} // namespace

double locsegChainPointDistLegacyLike(const LocsegChain& chain,
                                      const std::array<double, 3>& pos,
                                      int* segIndex,
                                      std::array<double, 3>* skelPos)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Point_Dist().
  const auto kaOpt = locsegChainToKnotArrayLegacyLike(chain);
  if (!kaOpt) {
    if (segIndex != nullptr) {
      *segIndex = -1;
    }
    return std::numeric_limits<double>::infinity();
  }

  const LocsegChainKnotArrayLegacyLike& ka = *kaOpt;
  const std::vector<Geo3dEllipseLegacyLike> ellipses =
    locsegChainKnotArrayToEllipseZLegacyLike(ka, /*zScaleFactor*/ 1.0);

  const int n = static_cast<int>(ka.knots.size());
  CHECK(n == static_cast<int>(ellipses.size()));
  if (n <= 0) {
    if (segIndex != nullptr) {
      *segIndex = -1;
    }
    return std::numeric_limits<double>::infinity();
  }

  Geo3dEllipseLegacyLike cur = ellipses[0];
  double minDist = geo3dEllipsePointDistanceLegacyLike(cur, pos);
  if (skelPos != nullptr) {
    *skelPos = cur.center;
  }

  int minIndex = 0;

  for (int i = 0; i < n - 1; ++i) {
    const double interval =
      coordinate3dDistance(ellipses[static_cast<size_t>(i)].center, ellipses[static_cast<size_t>(i + 1)].center);
    if (interval > 1.5) {
      const double dl = 1.0 / interval;
      for (double lambda = dl; lambda < 1.0; lambda += dl) {
        cur = geo3dEllipseInterpolateLegacyLike(ellipses[static_cast<size_t>(i)],
                                                ellipses[static_cast<size_t>(i + 1)],
                                                lambda);
        const double dist = geo3dEllipsePointDistanceLegacyLike(cur, pos);
        if (dist < minDist) {
          minDist = dist;
          const auto* knot = locsegChainKnotArrayAtLegacyLike(ka, i);
          CHECK(knot != nullptr);
          minIndex = knot->id;
          if (skelPos != nullptr) {
            *skelPos = cur.center;
          }
        }
      }

      const double distEnd = geo3dEllipsePointDistanceLegacyLike(ellipses[static_cast<size_t>(i + 1)], pos);
      if (distEnd < minDist) {
        minDist = distEnd;
        const auto* knot = locsegChainKnotArrayAtLegacyLike(ka, i + 1);
        CHECK(knot != nullptr);
        minIndex = knot->id;
        if (skelPos != nullptr) {
          *skelPos = ellipses[static_cast<size_t>(i + 1)].center;
        }
      }
    }
  }

  if (segIndex != nullptr) {
    *segIndex = minIndex;
  }

  return minDist;
}

void locsegChainBrightEndLegacyLike(const LocsegChain& chain,
                                    LocsegChainEndLegacyLike end,
                                    const ZImg& signal,
                                    double zToXYRatio,
                                    std::array<double, 3>& pos)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Bright_End().
  if (chain.empty()) {
    pos = {0.0, 0.0, 0.0};
    return;
  }

  const LocalNeuroseg* locseg = (end == LocsegChainEndLegacyLike::Head) ? chain.headSeg() : chain.tailSeg();
  CHECK(locseg != nullptr);

  double step = 0.0;
  double startOffset = 0.0;
  if (end == LocsegChainEndLegacyLike::Head) {
    step = 1.0;
    startOffset = 0.0;
  } else {
    step = -1.0;
    startOffset = locseg->seg.h - 1.0;
  }

  if (step == 0.0) {
    pos = {0.0, 0.0, 0.0};
    return;
  }

  double lastOffset = (locseg->seg.h - 1.0) / 2.0;
  if (lastOffset < 0.0) {
    LOG(WARNING) << "locsegChainBrightEndLegacyLike: negative lastOffset";
    lastOffset = 0.0;
  }

  pos = localNeurosegAxisPositionLegacyLike(*locseg, startOffset);
  std::array<double, 3> samplePos = pos;
  if (zToXYRatio != 1.0) {
    samplePos[2] /= zToXYRatio;
  }

  double maxValue = pointSampleLegacyLike(signal, samplePos[0], samplePos[1], samplePos[2]);
  double offset = startOffset + step;

  std::array<double, 3> tmpPos{};
  while (((end == LocsegChainEndLegacyLike::Head) && (offset <= lastOffset)) ||
         ((end == LocsegChainEndLegacyLike::Tail) && (offset >= lastOffset))) {
    tmpPos = localNeurosegAxisPositionLegacyLike(*locseg, offset);
    samplePos = tmpPos;
    if (zToXYRatio != 1.0) {
      samplePos[2] /= zToXYRatio;
    }
    const double value = pointSampleLegacyLike(signal, samplePos[0], samplePos[1], samplePos[2]);
    if (!std::isnan(value)) {
      if ((value > maxValue) || std::isnan(maxValue)) {
        maxValue = value;
        pos = tmpPos;
      }
    }
    offset += step;
  }
}

namespace {

void validateValue(int* v, int r)
{
  if (*v < 0) {
    *v = 0;
  } else if (*v >= r) {
    *v = r - 1;
  }
}

void validatePosition(std::array<int, 3>* pos, int width, int height, int depth)
{
  CHECK(pos != nullptr);
  validateValue(&(*pos)[0], width);
  validateValue(&(*pos)[1], height);
  validateValue(&(*pos)[2], depth);
}

void locsegChainPointRangeLegacyLike(const LocsegChain& target,
                                     int index,
                                     const std::array<double, 3>& pos,
                                     double dist,
                                     int* start,
                                     int* end)
{
  // Port of tz_locseg_chain.c::locseg_chain_point_range().
  CHECK(start != nullptr);
  CHECK(end != nullptr);

  const int length = target.length();
  CHECK(length > 0);

  if (index < 0) {
    index = 0;
  }
  if (index >= length) {
    index = length - 1;
  }

  auto segAt = [&](int idx) -> const LocalNeuroseg* {
    int i = 0;
    for (const auto& node : target) {
      if (i == idx) {
        return &node.locseg;
      }
      ++i;
    }
    return nullptr;
  };

  // Backward search.
  *start = index;
  const LocalNeuroseg* locseg = segAt(index);
  CHECK(locseg != nullptr);
  double d = coordinate3dDistance(pos, localNeurosegBottomLegacyLike(*locseg));

  int cur = index - 1;
  while (d < dist) {
    --(*start);
    if (cur >= 0) {
      const LocalNeuroseg* prev = segAt(cur);
      CHECK(prev != nullptr);
      d += coordinate3dDistance(pos, localNeurosegBottomLegacyLike(*prev));
      --cur;
    } else {
      break;
    }
  }

  // Forward search.
  *end = index;
  d = coordinate3dDistance(pos, localNeurosegTopLegacyLike(*locseg));

  cur = index + 1;
  while (d < dist) {
    ++(*end);
    if (cur < length) {
      const LocalNeuroseg* next = segAt(cur);
      CHECK(next != nullptr);
      d += coordinate3dDistance(pos, localNeurosegTopLegacyLike(*next));
      ++cur;
    } else {
      break;
    }
  }
}

[[nodiscard]] ZImg cropStackLegacyLike(const ZImg& src, const std::array<int, 6>& r)
{
  CHECK(!src.isEmpty());
  CHECK(src.numChannels() == 1);
  CHECK(src.numTimes() == 1);

  const int x0 = r[0];
  const int x1 = r[1];
  const int y0 = r[2];
  const int y1 = r[3];
  const int z0 = r[4];
  const int z1 = r[5];

  CHECK(x1 >= x0);
  CHECK(y1 >= y0);
  CHECK(z1 >= z0);

  const size_t w = static_cast<size_t>(x1 - x0 + 1);
  const size_t h = static_cast<size_t>(y1 - y0 + 1);
  const size_t d = static_cast<size_t>(z1 - z0 + 1);

  ZImgInfo info = src.info();
  info.setSize(Dimension::X, w);
  info.setSize(Dimension::Y, h);
  info.setSize(Dimension::Z, d);
  info.setSize(Dimension::C, 1);
  info.setSize(Dimension::T, 1);

  ZImg out(info);

  const size_t srcW = src.width();
  const size_t srcH = src.height();
  const size_t srcPlane = srcW * srcH;
  const size_t dstPlane = w * h;

  imgTypeDispatcher(src.info(), [&]<typename TVoxel>() {
    const auto* in = src.timeData<TVoxel>(0);
    auto* dst = out.timeData<TVoxel>(0);
    for (int zz = z0; zz <= z1; ++zz) {
      for (int yy = y0; yy <= y1; ++yy) {
        const size_t srcOff =
          static_cast<size_t>(x0) + static_cast<size_t>(yy) * srcW + static_cast<size_t>(zz) * srcPlane;
        const size_t dstOff = static_cast<size_t>(yy - y0) * w + static_cast<size_t>(zz - z0) * dstPlane;
        std::memcpy(dst + dstOff, in + srcOff, w * sizeof(TVoxel));
      }
    }
  });
  return out;
}

[[nodiscard]] ZImg cropMaskU8LegacyLike(const ZImg& src, const std::array<int, 6>& r)
{
  CHECK(src.isType<uint8_t>()) << src.info();
  return cropStackLegacyLike(src, r);
}

} // namespace

void locsegChainUpdateStackGraphWorkspaceLegacyLike(const LocalNeuroseg& source,
                                                    const LocsegChain& target,
                                                    const ZImg& signal,
                                                    double zToXYRatio,
                                                    StackGraphWorkspaceLegacyLike& sgw)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Update_Stack_Graph_Workspace().
  std::array<double, 3> pos = localNeurosegCenterLegacyLike(source);
  std::array<double, 3> imagePos = pos;
  if (zToXYRatio != 1.0) {
    imagePos[2] /= zToXYRatio;
  }

  int segIndex = 0;
  std::array<double, 3> skelPos{};
  (void)locsegChainPointDistLegacyLike(target, pos, &segIndex, &skelPos);

  int start = 0;
  int end = 0;
  locsegChainPointRangeLegacyLike(target, segIndex, skelPos, NeurosegDefaultHLegacyLike * 2.5, &start, &end);

  if (start < 0) {
    start = 0;
  }
  const int length = target.length();
  if (end >= length) {
    end = length - 1;
  }

  LocsegLabelWorkspaceLegacyLike ws;
  if (sgw.spOption != 1) {
    if (!sgw.groupMask) {
      ZImgInfo info(signal.width(), signal.height(), signal.depth(), 1, 1, 1, VoxelFormat::Unsigned);
      info.setVoxelFormat<uint8_t>();
      info.createDefaultDescriptions();
      sgw.groupMask.emplace(info);
    }
    sgw.groupMask->fill(0);
    ws.flag = 0;
    ws.value = 1;
    locsegChainLabelWLegacyLike(target, *sgw.groupMask, zToXYRatio, start, end, ws);
  }

  stackGraphWorkspaceSetRangeLegacyLike(sgw,
                                        static_cast<int>(imagePos[0]),
                                        ws.range[0],
                                        static_cast<int>(imagePos[1]),
                                        ws.range[1],
                                        static_cast<int>(imagePos[2]),
                                        ws.range[2]);
  stackGraphWorkspaceUpdateRangeLegacyLike(sgw, ws.range[3], ws.range[4], ws.range[5]);

  sgw.weightFunc = &stackVoxelWeightSLegacyLike;

  if (std::isnan(sgw.argv[3]) || std::isnan(sgw.argv[4])) {
    int option = 0;
    if (!std::isnan(sgw.argv[5])) {
      option = iroundLegacyLike(sgw.argv[5]);
    }

    switch (option) {
      case 0: {
        CHECK(sgw.range.has_value());
        const std::array<int, 6>& r = *sgw.range;

        const ZImg substack = cropStackLegacyLike(signal, r);
        std::optional<IntHistogramLegacyLike> histOpt;
        if (sgw.signalMask != nullptr) {
          const ZImg submask = cropMaskU8LegacyLike(*sgw.signalMask, r);
          histOpt = imageHistogramLegacyLike(substack, &submask);
        } else {
          histOpt = imageHistogramLegacyLike(substack, nullptr);
        }

        if (!histOpt) {
          break;
        }

        const IntHistogramLegacyLike& hist = *histOpt;
        double c1 = 0.0;
        double c2 = 0.0;
        const int thre = rcthreRLegacyLike(hist, hist.minValue(), hist.maxValue(), c1, c2);
        sgw.argv[3] = static_cast<double>(thre);
        sgw.argv[4] = c2 - c1;
        if (sgw.argv[4] < 1.0) {
          sgw.argv[4] = 1.0;
        }
        sgw.argv[4] /= 9.2;
        break;
      }

      case 1: {
        StackFitScore fs{};
        fs.n = 1;
        fs.options[0] = static_cast<int>(StackFitOption::MeanSignal);
        const double inner = localNeurosegScorePLegacyLike(source, signal, zToXYRatio, &fs);
        fs.options[0] = static_cast<int>(StackFitOption::OuterSignal);
        const double outer = localNeurosegScorePLegacyLike(source, signal, zToXYRatio, &fs);

        if (std::isnan(sgw.argv[3])) {
          sgw.argv[3] = inner * 0.1 + outer * 0.9;
        }

        if (std::isnan(sgw.argv[4])) {
          sgw.argv[4] = (inner - outer) / 4.6 * 1.8;
        }
        break;
      }

      default:
        break;
    }
  }
}

std::vector<int64_t> locsegChainShortestPathPtLegacyLike(std::array<double, 3> pos,
                                                         const LocsegChain& target,
                                                         int startIndex,
                                                         int endIndex,
                                                         const ZImg& signal,
                                                         double zToXYRatio,
                                                         StackGraphWorkspaceLegacyLike& sgw)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Shortest_Path_Pt().
  if (startIndex < 0) {
    startIndex = 0;
  }
  const int length = target.length();
  if (endIndex >= length) {
    endIndex = length - 1;
  }

  if (zToXYRatio != 1.0) {
    pos[2] /= zToXYRatio;
  }
  std::array<int, 3> startPos = {iroundLegacyLike(pos[0]), iroundLegacyLike(pos[1]), iroundLegacyLike(pos[2])};

  int segIndex = (startIndex + endIndex) / 2;
  const LocalNeuroseg* locseg = nullptr;
  {
    int i = 0;
    for (const auto& node : target) {
      if (i == segIndex) {
        locseg = &node.locseg;
        break;
      }
      ++i;
    }
  }
  CHECK(locseg != nullptr);
  pos = localNeurosegCenterLegacyLike(*locseg);

  if (zToXYRatio != 1.0) {
    pos[2] /= zToXYRatio;
  }

  std::array<int, 3> endPos = {iroundLegacyLike(pos[0]), iroundLegacyLike(pos[1]), iroundLegacyLike(pos[2])};

  LocsegLabelWorkspaceLegacyLike ws;
  if (sgw.spOption != 1) {
    if (!sgw.groupMask) {
      ZImgInfo info(signal.width(), signal.height(), signal.depth(), 1, 1, 1, VoxelFormat::Unsigned);
      info.setVoxelFormat<uint8_t>();
      info.createDefaultDescriptions();
      sgw.groupMask.emplace(info);
    }
    sgw.groupMask->fill(0);
    ws.flag = 0;
    ws.value = 1;
    locsegChainLabelWLegacyLike(target, *sgw.groupMask, 1.0, startIndex, endIndex, ws);
    stackGraphWorkspaceSetRangeLegacyLike(sgw,
                                          startPos[0],
                                          ws.range[0],
                                          startPos[1],
                                          ws.range[1],
                                          startPos[2],
                                          ws.range[2]);
    stackGraphWorkspaceUpdateRangeLegacyLike(sgw, ws.range[3], ws.range[4], ws.range[5]);
  } else {
    stackGraphWorkspaceSetRangeLegacyLike(sgw, startPos[0], endPos[0], startPos[1], endPos[1], startPos[2], endPos[2]);
  }

  stackGraphWorkspaceExpandRangeLegacyLike(sgw, 10, 10, 10, 10, 10, 10);
  stackGraphWorkspaceValidateRangeLegacyLike(sgw,
                                             static_cast<int>(signal.width()),
                                             static_cast<int>(signal.height()),
                                             static_cast<int>(signal.depth()));

  validatePosition(&startPos,
                   static_cast<int>(signal.width()),
                   static_cast<int>(signal.height()),
                   static_cast<int>(signal.depth()));
  validatePosition(&endPos,
                   static_cast<int>(signal.width()),
                   static_cast<int>(signal.height()),
                   static_cast<int>(signal.depth()));

  std::vector<int64_t> offsetPath;
  const double volumeEstimate =
    std::fabs(static_cast<double>((endPos[2] - startPos[2]) * (endPos[1] - startPos[1]) * (endPos[0] - startPos[0])));
  if (volumeEstimate < 100000.0) {
    offsetPath = stackRouteLegacyLike(signal, startPos, endPos, sgw);
  } else {
    // Port of tz_locseg_chain.c::Locseg_Chain_Shortest_Path_Pt() spGrow fallback.
    // NOTE: legacy code does not propagate the spGrow cost into `Stack_Graph_Workspace::value`,
    // so we intentionally leave `sgw.value` untouched here for parity.
    const int width = static_cast<int>(signal.width());
    const int height = static_cast<int>(signal.height());
    const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
    const size_t nvoxel = signal.voxelNumber();

    auto toIndex = [width, area](const std::array<int, 3>& p) -> int64_t {
      return static_cast<int64_t>(p[0]) + static_cast<int64_t>(p[1]) * static_cast<int64_t>(width) +
             static_cast<int64_t>(p[2]) * area;
    };

    const int64_t startVoxelIndex = toIndex(startPos);
    const int64_t endVoxelIndex = toIndex(endPos);
    if (startVoxelIndex < 0 || endVoxelIndex < 0 || static_cast<size_t>(startVoxelIndex) >= nvoxel ||
        static_cast<size_t>(endVoxelIndex) >= nvoxel) {
      return {};
    }

    SpGrowWorkspace tmpsgw;
    tmpsgw.conn = 26;
    tmpsgw.weightFunc = &stackVoxelWeightSLegacyLike;
    tmpsgw.resolution = sgw.resolution;
    tmpsgw.mask.assign(nvoxel, 0);

    tmpsgw.mask[static_cast<size_t>(startVoxelIndex)] = SP_GROW_SOURCE;
    tmpsgw.mask[static_cast<size_t>(endVoxelIndex)] = SP_GROW_TARGET;

    stackSpGrowInferParameterLegacyLike(tmpsgw, signal);
    offsetPath = stackSpGrowPathLegacyLike(signal, tmpsgw);
  }

  const int64_t nvoxel = static_cast<int64_t>(signal.voxelNumber());
  std::vector<int64_t> path;
  path.reserve(offsetPath.size());
  for (const int64_t idx : offsetPath) {
    if (idx >= 0 && idx < nvoxel) {
      path.push_back(idx);
    }
  }
  return path;
}

std::vector<int64_t> locsegChainShortestPathLegacyLike(const LocsegChain& source,
                                                       const LocsegChain& target,
                                                       const ZImg& signal,
                                                       double zToXYRatio,
                                                       StackGraphWorkspaceLegacyLike& sgw)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Shortest_Path().
  std::array<double, 3> pos{};
  locsegChainBrightEndLegacyLike(source, LocsegChainEndLegacyLike::Head, signal, 1.0, pos);
  if (zToXYRatio != 1.0) {
    pos[2] *= (1.0 / zToXYRatio);
  }

  int segIndex = 0;
  std::array<double, 3> skelPos{};
  double dist = locsegChainPointDistLegacyLike(target, pos, &segIndex, &skelPos);

  std::array<double, 3> tmpPos{};
  locsegChainBrightEndLegacyLike(source, LocsegChainEndLegacyLike::Tail, signal, 1.0, tmpPos);
  if (zToXYRatio != 1.0) {
    tmpPos[2] *= (1.0 / zToXYRatio);
  }

  int tmpSegIndex = 0;
  std::array<double, 3> tmpSkelPos{};
  const double tmpDist = locsegChainPointDistLegacyLike(target, tmpPos, &tmpSegIndex, &tmpSkelPos);

  const LocalNeuroseg* sourceSeg = source.headSeg();
  if (tmpDist < dist) {
    dist = tmpDist;
    segIndex = tmpSegIndex;
    pos = tmpPos;
    skelPos = tmpSkelPos;
    sourceSeg = source.tailSeg();
  }

  if (!inCloseRange3(pos,
                     0,
                     static_cast<int>(signal.width()) - 1,
                     0,
                     static_cast<int>(signal.height()) - 1,
                     0,
                     static_cast<int>(signal.depth()) - 1)) {
    return {};
  }

  CHECK(sourceSeg != nullptr);

  locsegChainUpdateStackGraphWorkspaceLegacyLike(*sourceSeg, target, signal, 1.0, sgw);
  if (std::isnan(sgw.argv[3])) {
    double tmpc = 0.0;
    double c2 = locsegChainMinSegScoreLegacyLike(source, signal, zToXYRatio, StackFitOption::MeanSignal);
    tmpc = locsegChainMinSegScoreLegacyLike(target, signal, zToXYRatio, StackFitOption::MeanSignal);
    if (tmpc < c2) {
      c2 = tmpc;
    }
    double c1 = locsegChainMinSegScoreLegacyLike(source, signal, zToXYRatio, StackFitOption::OuterSignal);
    tmpc = locsegChainMinSegScoreLegacyLike(target, signal, zToXYRatio, StackFitOption::OuterSignal);
    if (tmpc < c1) {
      c1 = tmpc;
    }
    sgw.argv[3] = (c1 + c2) / 2.0;
    if (std::isnan(sgw.argv[4])) {
      sgw.argv[4] = c2 - c1;
      if (sgw.argv[4] < 1.0) {
        sgw.argv[4] = 1.0;
      }
      sgw.argv[4] /= 9.2;
    }
  }

  int startIndex = 0;
  int endIndex = 0;
  locsegChainPointRangeLegacyLike(target, segIndex, skelPos, NeurosegDefaultHLegacyLike * 2.5, &startIndex, &endIndex);

  return locsegChainShortestPathPtLegacyLike(pos, target, startIndex, endIndex, signal, zToXYRatio, sgw);
}

} // namespace nim
