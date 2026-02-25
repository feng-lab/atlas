#include "zneutubestackroute.h"

#include "zneutubeneighborhood.h"
#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim::neutube {

namespace {

[[nodiscard]] int iroundLegacyLike(double x)
{
  return static_cast<int>(std::lround(x));
}

[[nodiscard]] double stackArrayValueLegacyLike(const ZImg& stack, size_t idx)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    return static_cast<double>(stack.timeData<uint8_t>(0)[idx]);
  }
  if (stack.isType<uint16_t>()) {
    return static_cast<double>(stack.timeData<uint16_t>(0)[idx]);
  }

  CHECK(false) << "stackRouteLegacyLike: unsupported voxel type " << stack.info();
  return 0.0;
}

[[nodiscard]] bool maskPositiveLegacyLike(const ZImg& mask, size_t idx)
{
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);

  if (mask.isType<uint8_t>()) {
    return mask.timeData<uint8_t>(0)[idx] > 0;
  }
  if (mask.isType<uint16_t>()) {
    return mask.timeData<uint16_t>(0)[idx] > 0;
  }

  CHECK(false) << "stackRouteLegacyLike: unsupported signal mask voxel type " << mask.info();
  return false;
}

[[nodiscard]] double stackIntensityLegacyLike(double v, const StackGraphWorkspaceLegacyLike& sgw)
{
  // Port of tz_stack_graph.c::stack_intensity().
  v = v * sgw.greyFactor + sgw.greyOffset;
  if (v < 0.0) {
    v = 0.0;
  }
  return v;
}

// Heap is 1-based with a preserved element at index 0 (value unused).
// checked[v] meanings match legacy:
//  0  : unseen/unqueued
//  1  : finalized
//  >1 : in heap, position = checked[v] - 1
class IntHeapLegacyLike
{
public:
  void reset()
  {
    _heap.clear();
    _heap.push_back(0);
  }

  [[nodiscard]] bool hasElements() const
  {
    return _heap.size() > 1;
  }

  void add(int item, const std::vector<double>& value, std::vector<int>& checked)
  {
    CHECK(item >= 0);
    CHECK(static_cast<size_t>(item) < checked.size());

    if (_heap.empty()) {
      _heap.push_back(0);
    }

    _heap.push_back(item);
    checked[static_cast<size_t>(item)] = static_cast<int>(_heap.size());

    int childIndex = checked[static_cast<size_t>(item)] - 1;
    int parentIndex = childIndex / 2;
    while (parentIndex > 0) {
      if (valueAt(childIndex, value) < valueAt(parentIndex, value)) {
        heapSwap(childIndex, parentIndex, checked);
        childIndex = parentIndex;
        parentIndex = childIndex / 2;
      } else {
        break;
      }
    }
  }

  void update(int item, const std::vector<double>& value, std::vector<int>& checked)
  {
    CHECK(item >= 0);
    CHECK(static_cast<size_t>(item) < checked.size());
    CHECK(checked[static_cast<size_t>(item)] > 1);

    int childIndex = checked[static_cast<size_t>(item)] - 1;
    int parentIndex = childIndex / 2;
    while (parentIndex > 0) {
      if (valueAt(childIndex, value) < valueAt(parentIndex, value)) {
        heapSwap(childIndex, parentIndex, checked);
        childIndex = parentIndex;
        parentIndex = childIndex / 2;
      } else {
        break;
      }
    }
  }

  [[nodiscard]] int removeMin(const std::vector<double>& value, std::vector<int>& checked)
  {
    CHECK(_heap.size() > 1);

    const int root = _heap[1];
    checked[static_cast<size_t>(root)] = 0;

    _heap[1] = _heap.back();
    _heap.pop_back();

    if (_heap.size() > 1) {
      checked[static_cast<size_t>(_heap[1])] = 2;
    }

    int parentIndex = 1;
    int childIndex = smallChild(parentIndex, value);

    while (childIndex > 0) {
      if (valueAt(parentIndex, value) > valueAt(childIndex, value)) {
        heapSwap(parentIndex, childIndex, checked);
        parentIndex = childIndex;
        childIndex = smallChild(parentIndex, value);
      } else {
        break;
      }
    }

    return root;
  }

private:
  std::vector<int> _heap;

  [[nodiscard]] double valueAt(int heapIndex, const std::vector<double>& value) const
  {
    CHECK(heapIndex >= 0);
    CHECK(static_cast<size_t>(heapIndex) < _heap.size());
    const int item = _heap[static_cast<size_t>(heapIndex)];
    CHECK(item >= 0);
    CHECK(static_cast<size_t>(item) < value.size());
    return value[static_cast<size_t>(item)];
  }

  [[nodiscard]] int smallChild(int parentIndex, const std::vector<double>& value) const
  {
    const int left = parentIndex * 2;
    if (left >= static_cast<int>(_heap.size())) {
      return 0;
    }
    int idx = left;
    const int right = left + 1;
    if (right < static_cast<int>(_heap.size())) {
      if (valueAt(left, value) > valueAt(right, value)) {
        idx = right;
      }
    }
    return idx;
  }

  void heapSwap(int a, int b, std::vector<int>& checked)
  {
    std::swap(_heap[static_cast<size_t>(a)], _heap[static_cast<size_t>(b)]);
    checked[static_cast<size_t>(_heap[static_cast<size_t>(a)])] = a + 1;
    checked[static_cast<size_t>(_heap[static_cast<size_t>(b)])] = b + 1;
  }
};

[[nodiscard]] int extractMinLegacyLike(std::vector<double>& dist, std::vector<int>& checked, IntHeapLegacyLike& heap)
{
  if (!heap.hasElements()) {
    return -1;
  }

  const int minIdx = heap.removeMin(dist, checked);
  const double minVal = dist[static_cast<size_t>(minIdx)];
  if (minVal == std::numeric_limits<double>::infinity()) {
    return -1;
  }

  checked[static_cast<size_t>(minIdx)] = 1;
  return minIdx;
}

[[nodiscard]] std::vector<int> neighborOffsetLegacyLike(int conn, int width, int height)
{
  std::vector<int> off;
  off.resize(static_cast<size_t>(conn));

  const int area = width * height;
  const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
  CHECK(static_cast<int>(nb.size()) == conn);

  for (size_t i = 0; i < nb.size(); ++i) {
    const ZVoxelCoordinate& o = nb.offset(i);
    off[i] = static_cast<int>(o.x) + static_cast<int>(o.y) * width + static_cast<int>(o.z) * area;
  }

  return off;
}

[[nodiscard]] std::vector<int64_t> neighborOffsetI64LegacyLike(int conn, int width, int height)
{
  std::vector<int64_t> off;
  off.resize(static_cast<size_t>(conn));

  const int64_t w = static_cast<int64_t>(width);
  const int64_t a = static_cast<int64_t>(width) * static_cast<int64_t>(height);

  const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
  CHECK(static_cast<int>(nb.size()) == conn);

  for (size_t i = 0; i < nb.size(); ++i) {
    const ZVoxelCoordinate& o = nb.offset(i);
    off[i] = static_cast<int64_t>(o.x) + static_cast<int64_t>(o.y) * w + static_cast<int64_t>(o.z) * a;
  }

  return off;
}

[[nodiscard]] std::vector<double> neighborDistRLegacyLike(int conn, const std::array<double, 3>& res)
{
  std::vector<double> dist;
  dist.resize(static_cast<size_t>(conn));

  const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
  CHECK(static_cast<int>(nb.size()) == conn);

  for (size_t i = 0; i < nb.size(); ++i) {
    const ZVoxelCoordinate& o = nb.offset(i);
    const double dx = static_cast<double>(o.x) * res[0];
    const double dy = static_cast<double>(o.y) * res[1];
    const double dz = static_cast<double>(o.z) * res[2];
    dist[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  return dist;
}

struct NeighborEdge
{
  int v = -1;
  double w = 0.0;
};

[[nodiscard]] int stackUtilOffset(int x, int y, int z, int width, int height)
{
  const int area = width * height;
  return x + y * width + z * area;
}

[[nodiscard]] int64_t stackSubindexLegacyLike(int subIndex,
                                              int xOffset,
                                              int yOffset,
                                              int zOffset,
                                              int subWidth,
                                              int subArea,
                                              int width,
                                              int area)
{
  const int z = subIndex / subArea;
  const int rem = subIndex - z * subArea;
  const int y = rem / subWidth;
  const int x = rem - y * subWidth;
  return static_cast<int64_t>(x + xOffset) + static_cast<int64_t>(y + yOffset) * static_cast<int64_t>(width) +
         static_cast<int64_t>(z + zOffset) * static_cast<int64_t>(area);
}

} // namespace

std::vector<int64_t> stackRouteLegacyLike(const ZImg& stack,
                                          const std::array<int, 3>& startPos,
                                          const std::array<int, 3>& endPos,
                                          StackGraphWorkspaceLegacyLike* sgw)
{
  CHECK(sgw != nullptr);
  if (stack.isEmpty()) {
    sgw->value = std::numeric_limits<double>::infinity();
    return {};
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());
  const int64_t area = static_cast<int64_t>(width) * static_cast<int64_t>(height);

  if (!sgw->range.has_value()) {
    const double dist = geo3dDist(static_cast<double>(startPos[0]),
                                  static_cast<double>(startPos[1]),
                                  static_cast<double>(startPos[2]),
                                  static_cast<double>(endPos[0]),
                                  static_cast<double>(endPos[1]),
                                  static_cast<double>(endPos[2]));

    std::array<int, 3> margin{};
    for (int i = 0; i < 3; ++i) {
      margin[static_cast<size_t>(i)] =
        iroundLegacyLike(dist - std::abs(endPos[static_cast<size_t>(i)] - startPos[static_cast<size_t>(i)] + 1));
      if (margin[static_cast<size_t>(i)] < 0) {
        margin[static_cast<size_t>(i)] = 0;
      }
    }

    stackGraphWorkspaceSetRangeLegacyLike(sgw, startPos[0], endPos[0], startPos[1], endPos[1], startPos[2], endPos[2]);
    stackGraphWorkspaceExpandRangeLegacyLike(sgw, margin[0], margin[0], margin[1], margin[1], margin[2], margin[2]);
    stackGraphWorkspaceValidateRangeLegacyLike(sgw, width, height, depth);
  }

  CHECK(sgw->range.has_value());

  // Clamp to stack dims like Stack_Graph_W does.
  std::array<int, 6> stackRange = *sgw->range;
  stackRange[0] = std::max(0, stackRange[0]);
  stackRange[1] = std::min(width - 1, stackRange[1]);
  stackRange[2] = std::max(0, stackRange[2]);
  stackRange[3] = std::min(height - 1, stackRange[3]);
  stackRange[4] = std::max(0, stackRange[4]);
  stackRange[5] = std::min(depth - 1, stackRange[5]);

  const int swidth = stackRange[1] - stackRange[0] + 1;
  const int sheight = stackRange[3] - stackRange[2] + 1;
  const int sdepth = stackRange[5] - stackRange[4] + 1;
  const int sarea = swidth * sheight;
  const int svolume = sarea * sdepth;

  CHECK(swidth > 0);
  CHECK(sheight > 0);
  CHECK(sdepth > 0);

  int startIndex = stackUtilOffset(startPos[0] - stackRange[0],
                                   startPos[1] - stackRange[2],
                                   startPos[2] - stackRange[4],
                                   swidth,
                                   sheight);
  int endIndex =
    stackUtilOffset(endPos[0] - stackRange[0], endPos[1] - stackRange[2], endPos[2] - stackRange[4], swidth, sheight);

  if (startIndex > endIndex) {
    std::swap(startIndex, endIndex);
  }

  CHECK(startIndex >= 0);
  CHECK(endIndex >= 0);
  CHECK(startIndex < svolume);
  CHECK(endIndex < svolume);

  const int baseVertexCount = svolume;
  sgw->virtualVertex = baseVertexCount;

  std::array<int, 256> groupVertexMap{};
  groupVertexMap.fill(0);

  std::vector<std::vector<NeighborEdge>> adjacency;
  adjacency.resize(static_cast<size_t>(baseVertexCount));

  const std::vector<int> neighborSub = neighborOffsetLegacyLike(sgw->conn, swidth, sheight);
  const std::vector<int64_t> neighborOrg = neighborOffsetI64LegacyLike(sgw->conn, width, height);
  const std::vector<double> neighborDist = neighborDistRLegacyLike(sgw->conn, sgw->resolution);
  const ZNeighborhood& nb = neighborhoodLegacyOrder(sgw->conn);

  auto addUndirectedEdge = [&adjacency](int v1, int v2, double w) {
    CHECK(v1 >= 0);
    CHECK(v2 >= 0);
    CHECK(static_cast<size_t>(v1) < adjacency.size());
    CHECK(static_cast<size_t>(v2) < adjacency.size());
    adjacency[static_cast<size_t>(v1)].push_back(NeighborEdge{v2, w});
    adjacency[static_cast<size_t>(v2)].push_back(NeighborEdge{v1, w});
  };

  const bool useSignalMask = (sgw->signalMask != nullptr) && (!sgw->signalMask->isEmpty());
  if (useSignalMask) {
    CHECK(sgw->signalMask->isSameSize(stack))
      << "signalMask size mismatch: mask=" << sgw->signalMask->info() << " stack=" << stack.info();
  }

  const bool useGroupMask = sgw->groupMask.has_value() && (!sgw->groupMask->isEmpty());
  if (useGroupMask) {
    CHECK(sgw->groupMask->isSameSize(stack))
      << "groupMask size mismatch: mask=" << sgw->groupMask->info() << " stack=" << stack.info();
    CHECK(sgw->groupMask->isType<uint8_t>())
      << "groupMask must be uint8 (GREY) for legacy parity: " << sgw->groupMask->info();
  }

  int nextVertexId = baseVertexCount;
  int offset = 0;

  for (int z = 0; z < sdepth; ++z) {
    for (int y = 0; y < sheight; ++y) {
      for (int x = 0; x < swidth; ++x) {
        const int64_t offset2Signed = static_cast<int64_t>(x + stackRange[0]) +
                                      static_cast<int64_t>(y + stackRange[2]) * static_cast<int64_t>(width) +
                                      static_cast<int64_t>(z + stackRange[4]) * area;
        CHECK(offset2Signed >= 0);
        const size_t offset2 = static_cast<size_t>(offset2Signed);

        int nbound = 0;
        std::array<uint8_t, 26> isInBound{};
        for (size_t i = 0; i < nb.size(); ++i) {
          const ZVoxelCoordinate& o = nb.offset(i);
          const int nx = x + o.x;
          const int ny = y + o.y;
          const int nz = z + o.z;
          const bool in = (nx >= 0 && ny >= 0 && nz >= 0 && nx < swidth && ny < sheight && nz < sdepth);
          isInBound[i] = static_cast<uint8_t>(in);
          if (in) {
            ++nbound;
          }
        }

        const bool isFullyInBound = (nbound == sgw->conn);

        // Add forward voxel-voxel edges (legacy scan_mask: neighborSub[i] > 0).
        for (size_t i = 0; i < nb.size(); ++i) {
          if (neighborSub[i] <= 0) {
            continue;
          }
          if (!isFullyInBound && isInBound[i] == 0) {
            continue;
          }

          if (useSignalMask) {
            const int64_t nOffset2Signed = offset2Signed + neighborOrg[i];
            CHECK(nOffset2Signed >= 0);
            const size_t nOffset2 = static_cast<size_t>(nOffset2Signed);
            const bool a = maskPositiveLegacyLike(*sgw->signalMask, offset2);
            const bool b = maskPositiveLegacyLike(*sgw->signalMask, nOffset2);
            const bool ok = sgw->includingSignalBorder ? (a || b) : (a && b);
            if (!ok) {
              continue;
            }
          }

          double weight = neighborDist[i];
          if (sgw->weightFunc != nullptr) {
            sgw->argv[0] = neighborDist[i];

            double v1 = stackArrayValueLegacyLike(stack, offset2);
            const int64_t nOffset2Signed = offset2Signed + neighborOrg[i];
            CHECK(nOffset2Signed >= 0);
            const size_t nOffset2 = static_cast<size_t>(nOffset2Signed);
            double v2 = stackArrayValueLegacyLike(stack, nOffset2);
            if (sgw->greyFactor != 1.0 || sgw->greyOffset != 0.0) {
              v1 = stackIntensityLegacyLike(v1, *sgw);
              v2 = stackIntensityLegacyLike(v2, *sgw);
            }

            sgw->argv[1] = v1;
            sgw->argv[2] = v2;

            weight = sgw->weightFunc(sgw->argv.data());
          }

          addUndirectedEdge(offset, offset + neighborSub[i], weight);
        }

        if (useGroupMask) {
          const int groupId = static_cast<int>(sgw->groupMask->timeData<uint8_t>(0)[offset2]);
          if (groupId > 0 && groupId < static_cast<int>(groupVertexMap.size())) {
            int groupVertex = groupVertexMap[static_cast<size_t>(groupId)];
            if (groupVertex <= 0) {
              groupVertex = nextVertexId++;
              groupVertexMap[static_cast<size_t>(groupId)] = groupVertex;
              adjacency.resize(static_cast<size_t>(nextVertexId));
            }

            addUndirectedEdge(groupVertex, offset, 0.0);
          }
        }

        ++offset;
      }
    }
  }

  const int nvertex = nextVertexId;

  // Dijkstra (Graph_Shortest_Path_E semantics).
  std::vector<double> dist(static_cast<size_t>(nvertex), std::numeric_limits<double>::infinity());
  std::vector<int> path(static_cast<size_t>(nvertex), -1);
  std::vector<int> checked(static_cast<size_t>(nvertex), 0);

  dist[static_cast<size_t>(startIndex)] = 0.0;
  path[static_cast<size_t>(startIndex)] = -1;
  checked[static_cast<size_t>(startIndex)] = 1;

  IntHeapLegacyLike heap;
  heap.reset();

  int curVertex = startIndex;

  for (int iter = 1; iter < nvertex; ++iter) {
    for (const NeighborEdge& e : adjacency[static_cast<size_t>(curVertex)]) {
      const int updatingVertex = e.v;
      if (checked[static_cast<size_t>(updatingVertex)] == 1) {
        continue;
      }

      const double tmpdist = e.w + dist[static_cast<size_t>(curVertex)];
      if (dist[static_cast<size_t>(updatingVertex)] > tmpdist) {
        dist[static_cast<size_t>(updatingVertex)] = tmpdist;
        path[static_cast<size_t>(updatingVertex)] = curVertex;

        if (checked[static_cast<size_t>(updatingVertex)] > 1) {
          heap.update(updatingVertex, dist, checked);
        } else {
          heap.add(updatingVertex, dist, checked);
        }
      }
    }

    curVertex = extractMinLegacyLike(dist, checked, heap);
    if (curVertex == endIndex) {
      break;
    }
    if (curVertex < 0) {
      break;
    }
  }

  sgw->value = dist[static_cast<size_t>(endIndex)];
  if (sgw->value == std::numeric_limits<double>::infinity()) {
    return {};
  }

  // Parse_Stack_Shortest_Path.
  std::vector<int64_t> offsetPath;
  offsetPath.reserve(256);

  int end = endIndex;
  while (end >= 0) {
    int64_t index = -1;
    if (end < svolume) {
      index = stackSubindexLegacyLike(end, stackRange[0], stackRange[2], stackRange[4], swidth, sarea, width, area);
    }
    offsetPath.push_back(index);
    end = path[static_cast<size_t>(end)];
  }

  const int64_t orgStart = static_cast<int64_t>(stackUtilOffset(startPos[0], startPos[1], startPos[2], width, height));
  if (!offsetPath.empty() && orgStart != offsetPath.front()) {
    std::reverse(offsetPath.begin(), offsetPath.end());
  }

  const int64_t orgEnd = static_cast<int64_t>(stackUtilOffset(endPos[0], endPos[1], endPos[2], width, height));
  CHECK(!offsetPath.empty());
  CHECK(orgStart == offsetPath.front()) << "Wrong path head. orgStart=" << orgStart << " got=" << offsetPath.front();
  CHECK(orgEnd == offsetPath.back()) << "Wrong path tail. orgEnd=" << orgEnd << " got=" << offsetPath.back();

  return offsetPath;
}

} // namespace nim::neutube
