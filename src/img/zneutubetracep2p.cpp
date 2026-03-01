#include "zneutubetracep2p.h"

#include "zneutubeinthistogram.h"
#include "zneutubestackgraph.h"
#include "zneutubestackroute.h"

#include "zexception.h"
#include "zlog.h"
#include "zswcnodeops.h"
#include "zswcops.h"
#include "zswcresampler.h"
#include "zswctreenodegeomlegacy.h"
#include "zneutubevoxel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace nim {

namespace {

constexpr int MaxP2PTraceVolumeLegacyLike = 1000000;

[[nodiscard]] int iroundLegacyLike(double v)
{
  return static_cast<int>(std::lround(v));
}

[[nodiscard]] ZImg traceSignalViewLegacyLike(const ZImg& signal, size_t c, size_t t)
{
  if (signal.isEmpty()) {
    return {};
  }

  const ZImgInfo info = signal.info();
  if (c >= info.numChannels || t >= info.numTimes) {
    throw ZException(
      fmt::format("P2P trace: invalid channel/time selection (c={}, t={}) for signal <{}>.", c, t, info));
  }

  return signal.createView(static_cast<index_t>(c), static_cast<index_t>(t));
}

void inferWeightParameterLegacyLike(StackGraphWorkspaceLegacyLike& sgw, const ZImg& stack)
{
  if (sgw.weightFunc != &stackVoxelWeightSLegacyLike && sgw.weightFunc != &stackVoxelWeightSrLegacyLike) {
    return;
  }

  const auto histOpt = imageHistogramLegacyLike(stack, nullptr);
  if (!histOpt) {
    return;
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
}

[[nodiscard]] ZImg cropByRange(const ZImg& stack, const std::array<int, 6>& range)
{
  const int x0 = range[0];
  const int x1 = range[1];
  const int y0 = range[2];
  const int y1 = range[3];
  const int z0 = range[4];
  const int z1 = range[5];

  const ZVoxelCoordinate start(static_cast<index_t>(x0), static_cast<index_t>(y0), static_cast<index_t>(z0), 0, 0);
  const ZVoxelCoordinate end(static_cast<index_t>(x1 + 1),
                             static_cast<index_t>(y1 + 1),
                             static_cast<index_t>(z1 + 1),
                             1,
                             1);
  return stack.crop(ZImgRegion(start, end));
}

template<typename TVoxel>
[[nodiscard]] int valueAt(const TVoxel* data, size_t idx)
{
  return static_cast<int>(data[idx]);
}

[[nodiscard]] ZImg computeGradientLegacyLike(const ZImg& in)
{
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);

  const int width = static_cast<int>(in.width());
  const int height = static_cast<int>(in.height());
  const int depth = static_cast<int>(in.depth());
  if (width <= 0 || height <= 0 || depth <= 0) {
    return {};
  }

  ZImgInfo outInfo(static_cast<size_t>(width),
                   static_cast<size_t>(height),
                   static_cast<size_t>(depth),
                   1,
                   1,
                   1,
                   VoxelFormat::Unsigned);
  outInfo.setVoxelFormat<uint8_t>();
  outInfo.createDefaultDescriptions();

  ZImg out(outInfo);
  out.fill(0);

  const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  const int cwidth = width - 1;
  const int cheight = height - 1;
  const int cdepth = depth - 1;

  auto compute = [&](const auto* src) {
    uint8_t* dst = out.timeData<uint8_t>(0);

    if (cdepth == 0) {
      int64_t offset = width;
      for (int y = 1; y < cheight; ++y) {
        ++offset;
        for (int x = 1; x < cwidth; ++x) {
          const int dx = valueAt(src, static_cast<size_t>(offset + 1)) - valueAt(src, static_cast<size_t>(offset - 1));
          const int dy =
            valueAt(src, static_cast<size_t>(offset + width)) - valueAt(src, static_cast<size_t>(offset - width));
          const int v = iroundLegacyLike(std::sqrt((static_cast<double>(dx * dx + dy * dy)) / 8.0));
          dst[offset++] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
        ++offset;
      }
    } else {
      int64_t offset = area;
      for (int z = 1; z < cdepth; ++z) {
        offset += width;
        for (int y = 1; y < cheight; ++y) {
          ++offset;
          for (int x = 1; x < cwidth; ++x) {
            const int dx =
              valueAt(src, static_cast<size_t>(offset + 1)) - valueAt(src, static_cast<size_t>(offset - 1));
            const int dy =
              valueAt(src, static_cast<size_t>(offset + width)) - valueAt(src, static_cast<size_t>(offset - width));
            const int dz =
              valueAt(src, static_cast<size_t>(offset + area)) - valueAt(src, static_cast<size_t>(offset - area));
            const int v = iroundLegacyLike(std::sqrt((static_cast<double>(dx * dx + dy * dy + dz * dz)) / 12.0));
            dst[offset++] = static_cast<uint8_t>(std::clamp(v, 0, 255));
          }
          ++offset;
        }
        offset += width;
      }
    }
  };

  if (in.isType<uint8_t>()) {
    compute(in.timeData<uint8_t>(0));
  } else if (in.isType<uint16_t>()) {
    compute(in.timeData<uint16_t>(0));
  } else {
    throw ZException(fmt::format("P2P trace: unsupported voxel type {}", in.info()));
  }

  return out;
}

void updateRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw,
                           int x1,
                           int y1,
                           int z1,
                           int x2,
                           int y2,
                           int z2,
                           int width,
                           int height,
                           int depth,
                           int zMargin)
{
  if (sgw.range.has_value()) {
    return;
  }

  const double dx = static_cast<double>(x1 - x2);
  const double dy = static_cast<double>(y1 - y2);
  const double dz = static_cast<double>(z1 - z2);
  const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);

  std::array<int, 3> margin{};
  margin[0] = iroundLegacyLike(dist / 2.0 - std::abs(x2 - x1 + 1));
  margin[1] = iroundLegacyLike(dist / 2.0 - std::abs(y2 - y1 + 1));
  if (zMargin < 0) {
    margin[2] = iroundLegacyLike(dist / 4.0 - std::abs(z2 - z1 + 1));
  } else {
    margin[2] = zMargin;
  }

  for (int i = 0; i < 3; ++i) {
    if (margin[i] < 0) {
      margin[i] = 0;
    }
  }

  stackGraphWorkspaceSetRangeLegacyLike(sgw, x1, x2, y1, y2, z1, z2);
  stackGraphWorkspaceExpandRangeLegacyLike(sgw, margin[0], margin[0], margin[1], margin[1], margin[2], margin[2]);
  stackGraphWorkspaceValidateRangeLegacyLike(sgw, width, height, depth);
}

[[nodiscard]] size_t roiVolumeLegacyLike(const std::array<int, 6>& r)
{
  const int w = r[1] - r[0] + 1;
  const int h = r[3] - r[2] + 1;
  const int d = r[5] - r[4] + 1;
  if (w <= 0 || h <= 0 || d <= 0) {
    return 0;
  }
  return static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(d);
}

[[nodiscard]] std::vector<ZNeutubeVoxel> pathIndicesToVoxels(const ZImg& stack, const std::vector<int64_t>& pathIndices)
{
  std::vector<ZNeutubeVoxel> voxels;
  if (pathIndices.empty()) {
    return voxels;
  }

  const int64_t nvoxel = static_cast<int64_t>(stack.voxelNumber());
  voxels.reserve(pathIndices.size());

  for (auto it = pathIndices.rbegin(); it != pathIndices.rend(); ++it) {
    const int64_t idx = *it;
    if (idx < 0 || idx >= nvoxel) {
      continue;
    }
    const ZVoxelCoordinate c = stack.indexToCoord(static_cast<index_t>(idx));
    voxels.emplace_back(static_cast<int>(c.x), static_cast<int>(c.y), static_cast<int>(c.z), 0.0);
  }

  return voxels;
}

void interpolateRadiiLegacyLike(std::vector<ZNeutubeVoxel>& voxels, double r1, double r2)
{
  if (voxels.empty()) {
    return;
  }

  double length = 0.0;
  for (size_t i = 1; i < voxels.size(); ++i) {
    length += voxels[i].distanceTo(voxels[i - 1]);
  }

  if (length <= 0.0) {
    for (auto& v : voxels) {
      v.value = r2;
    }
    return;
  }

  double dist = 0.0;
  for (size_t i = 0; i < voxels.size(); ++i) {
    const double ratio = dist / length;
    const double r = r1 * ratio + r2 * (1.0 - ratio);
    voxels[i].value = r;
    if (i + 1 < voxels.size()) {
      dist += voxels[i].distanceTo(voxels[i + 1]);
    }
  }
}

[[nodiscard]] std::unique_ptr<ZSwc> voxelsToSwcLegacyLike(const std::vector<ZNeutubeVoxel>& voxels)
{
  if (voxels.empty()) {
    return nullptr;
  }

  auto swc = std::make_unique<ZSwc>();

  auto nodeFromVoxel = [](const ZNeutubeVoxel& v) -> SwcNode {
    SwcNode n;
    n.id = 0;
    n.type = 0;
    n.x = static_cast<double>(v.x);
    n.y = static_cast<double>(v.y);
    n.z = static_cast<double>(v.z);
    n.radius = v.value;
    n.parentID = -1;
    n.selected = false;
    return n;
  };

  // Root is the last voxel in the list (legacy ZVoxelArray::toSwcTree sets root=endIndex).
  ZSwc::SwcTreeNode parent = swc->appendRoot(nodeFromVoxel(voxels.back()));
  for (size_t i = voxels.size() - 1; i-- > 0;) {
    parent = swc->appendChild(parent, nodeFromVoxel(voxels[i]));
  }

  // Legacy end-case: if root and its child overlap and the child has its own child, merge the child into the root.
  const ZSwc::SwcTreeNode root = swc->beginRoot();
  const ZSwc::SwcTreeNode first = ZSwc::firstChild(root);
  if (!ZSwc::isNull(first)) {
    const ZSwc::SwcTreeNode second = ZSwc::firstChild(first);
    if (!ZSwc::isNull(second) && swcNodesHasOverlapLegacyLike(first, root)) {
      mergeToParent(*swc, first);
    }
  }

  ZNeutubeSwcResampler resampler;
  resampler.ignoreInterRedundant(true);
  resampler.optimalDownsample(*swc);

  return swc;
}

void fitSignalOnTreeLegacyLike(ZSwc& tree, const ZImg& stack, ZNeutubeImageBackgroundLegacyLike bg, bool fixTerminal)
{
  if (tree.empty()) {
    return;
  }
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const ZSwc::SwcTreeNode root = tree.beginRoot();

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    if (it == root) {
      continue;
    }

    const glm::dvec3 oldCenter(it->x, it->y, it->z);
    const double oldBend = swcNodeMaxBendingEnergyLegacyLike(tree, it);

    SwcNode nodeCopy = *it;
    (void)fitSwcNodeSignalWithFallbackLegacyLike(nodeCopy, stack, bg);

    it->x = nodeCopy.x;
    it->y = nodeCopy.y;
    it->radius = nodeCopy.radius;

    if (fixTerminal && isLeaf(it)) {
      it->x = oldCenter.x;
      it->y = oldCenter.y;
      it->z = oldCenter.z;
    }

    const double newBend = swcNodeMaxBendingEnergyLegacyLike(tree, it);
    if (newBend > 1.0) {
      if (newBend - oldBend > 0.5) {
        it->x = oldCenter.x;
        it->y = oldCenter.y;
        it->z = oldCenter.z;
      }
    }
  }
}

} // namespace

std::unique_ptr<ZSwc> tracePointToPointLegacyLike(const ZImg& signal,
                                                  size_t c,
                                                  size_t t,
                                                  const std::array<double, 3>& start,
                                                  double startRadius,
                                                  const std::array<double, 3>& target,
                                                  double targetRadius,
                                                  const TraceConfig& cfg,
                                                  ZNeutubeImageBackgroundLegacyLike background)
{
  if (signal.isEmpty()) {
    return nullptr;
  }

  const std::array<double, 3> targetPos = target;

  int x1 = iroundLegacyLike(start[0]);
  int y1 = iroundLegacyLike(start[1]);
  int z1 = iroundLegacyLike(start[2]);
  int x2 = iroundLegacyLike(target[0]);
  int y2 = iroundLegacyLike(target[1]);
  int z2 = iroundLegacyLike(target[2]);

  const ZImg stackView = traceSignalViewLegacyLike(signal, c, t);
  if (stackView.isEmpty()) {
    return nullptr;
  }

  const int width = static_cast<int>(stackView.width());
  const int height = static_cast<int>(stackView.height());
  const int depth = static_cast<int>(stackView.depth());

  if (x1 < 0 || y1 < 0 || z1 < 0 || x1 >= width || y1 >= height || z1 >= depth) {
    return nullptr;
  }

  StackGraphWorkspaceLegacyLike sgw;
  defaultStackGraphWorkspaceLegacyLike(sgw);

  const ZImgInfo info = stackView.info();
  sgw.resolution = {info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ};

  int zMargin = -1;
  if (sgw.resolution[0] > 0.0 && (sgw.resolution[2] / sgw.resolution[0] > 3.0)) {
    zMargin = 2;
  }

  if (background == ZNeutubeImageBackgroundLegacyLike::Bright) {
    sgw.weightFunc = &stackVoxelWeightSrLegacyLike;
  } else {
    sgw.weightFunc = &stackVoxelWeightSLegacyLike;
  }

  updateRangeLegacyLike(sgw, x1, y1, z1, x2, y2, z2, width, height, depth, zMargin);
  if (!sgw.range.has_value()) {
    return nullptr;
  }

  const std::array<int, 6> range = *sgw.range;
  if (roiVolumeLegacyLike(range) > static_cast<size_t>(MaxP2PTraceVolumeLegacyLike)) {
    return nullptr;
  }

  if (cfg.edgePath) {
    const int x0 = range[0];
    const int y0 = range[2];
    const int z0 = range[4];

    ZImg partial = cropByRange(stackView, range);
    if (partial.isEmpty()) {
      return nullptr;
    }

    ZImg partialEdge = computeGradientLegacyLike(partial);
    partial = {};

    inferWeightParameterLegacyLike(sgw, partialEdge);

    const int pw = static_cast<int>(partialEdge.width());
    const int ph = static_cast<int>(partialEdge.height());
    const int pd = static_cast<int>(partialEdge.depth());

    stackGraphWorkspaceSetRangeLegacyLike(sgw, 0, pw - 1, 0, ph - 1, 0, pd - 1);

    const std::array<int, 3> startPos = {x1 - x0, y1 - y0, z1 - z0};
    const std::array<int, 3> endPos = {x2 - x0, y2 - y0, z2 - z0};

    std::vector<int64_t> path = stackRouteLegacyLike(partialEdge, startPos, endPos, sgw);
    std::vector<ZNeutubeVoxel> voxels = pathIndicesToVoxels(partialEdge, path);
    for (auto& v : voxels) {
      v.translate(x0, y0, z0);
    }

    interpolateRadiiLegacyLike(voxels, startRadius, targetRadius);
    std::unique_ptr<ZSwc> tree = voxelsToSwcLegacyLike(voxels);
    if (!tree || tree->empty()) {
      return nullptr;
    }

    fitSignalOnTreeLegacyLike(*tree, stackView, background, /*fixTerminal*/ true);

    // Force leaf position back to the original target coordinates.
    ZSwc::SwcTreeNode leaf = tree->beginRoot();
    while (true) {
      const auto child = ZSwc::firstChild(leaf);
      if (ZSwc::isNull(child)) {
        break;
      }
      leaf = child;
    }
    leaf->x = targetPos[0];
    leaf->y = targetPos[1];
    leaf->z = targetPos[2];

    return tree;
  }

  // Non-edge-path: infer weights on cropped ROI but route on the full stack view.
  {
    const ZImg partial = cropByRange(stackView, range);
    if (!partial.isEmpty()) {
      inferWeightParameterLegacyLike(sgw, partial);
    }
  }

  const std::array<int, 3> startPos = {x1, y1, z1};
  const std::array<int, 3> endPos = {x2, y2, z2};

  std::vector<int64_t> path = stackRouteLegacyLike(stackView, startPos, endPos, sgw);
  std::vector<ZNeutubeVoxel> voxels = pathIndicesToVoxels(stackView, path);

  interpolateRadiiLegacyLike(voxels, startRadius, targetRadius);
  std::unique_ptr<ZSwc> tree = voxelsToSwcLegacyLike(voxels);
  if (!tree || tree->empty()) {
    return nullptr;
  }

  fitSignalOnTreeLegacyLike(*tree, stackView, background, /*fixTerminal*/ true);

  ZSwc::SwcTreeNode leaf = tree->beginRoot();
  while (true) {
    const auto child = ZSwc::firstChild(leaf);
    if (ZSwc::isNull(child)) {
      break;
    }
    leaf = child;
  }
  leaf->x = targetPos[0];
  leaf->y = targetPos[1];
  leaf->z = targetPos[2];

  return tree;
}

} // namespace nim
