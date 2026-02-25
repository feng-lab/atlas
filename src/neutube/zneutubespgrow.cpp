#include "zneutubespgrow.h"

#include "zneutubeinthistogram.h"
#include "zneutubestackgraph.h"

#include "zneutubeneighborhood.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace nim::neutube {

double stackVoxelWeightILegacyLike(double* argv)
{
  CHECK(argv != nullptr);

  const double d = argv[0];
  const double v1 = argv[1];
  const double v2 = argv[2];

  // Legacy Stack_Voxel_Weight_I:
  // return d / (1.0 + v1) + d / (v2 + 1.0);
  return d / (1.0 + v1) + d / (1.0 + v2);
}

void stackSpGrowInferParameterLegacyLike(SpGrowWorkspace* sgw, const ZImg& stack)
{
  CHECK(sgw != nullptr);

  // Port of tz_sp_grow.c::Stack_Sp_Grow_Infer_Parameter().
  if (sgw->weightFunc != &stackVoxelWeightSLegacyLike) {
    return;
  }

  const auto histOpt = imageHistogramLegacyLike(stack, nullptr);
  if (!histOpt) {
    return;
  }

  const IntHistogramLegacyLike& hist = *histOpt;

  double c1 = 0.0;
  double c2 = 0.0;
  const int thre = rcthreRLegacyLike(hist, hist.minValue(), hist.maxValue(), &c1, &c2);

  sgw->argv[3] = static_cast<double>(thre);
  sgw->argv[4] = c2 - c1;
  if (sgw->argv[4] < 1.0) {
    sgw->argv[4] = 1.0;
  }
  sgw->argv[4] /= 9.2;
}

namespace {

[[nodiscard]] double stackArrayValue(const ZImg& stack, size_t idx)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (stack.isType<uint8_t>()) {
    return static_cast<double>(stack.timeData<uint8_t>(0)[idx]);
  }
  if (stack.isType<uint16_t>()) {
    return static_cast<double>(stack.timeData<uint16_t>(0)[idx]);
  }

  throw ZException(fmt::format("stackSpGrow: unsupported voxel type {}", stack.info()));
}

[[nodiscard]] std::vector<double> neighborDistR(int conn, const std::array<double, 3>& res)
{
  std::vector<double> dist;
  dist.resize(static_cast<size_t>(conn));

  if (res[0] <= 0.0 || res[1] <= 0.0 || res[2] <= 0.0) {
    std::fill(dist.begin(), dist.end(), 1.0);
    return dist;
  }

  // Compute using the legacy neighbor ordering.
  const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
  for (size_t i = 0; i < nb.size(); ++i) {
    const ZVoxelCoordinate& o = nb.offset(i);
    const double dx = static_cast<double>(o.x) * res[0];
    const double dy = static_cast<double>(o.y) * res[1];
    const double dz = static_cast<double>(o.z) * res[2];
    dist[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  return dist;
}

[[nodiscard]] std::vector<int> neighborOffset(int conn, int width, int height)
{
  std::vector<int> off;
  off.resize(static_cast<size_t>(conn));
  const int area = width * height;

  const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
  for (size_t i = 0; i < nb.size(); ++i) {
    const ZVoxelCoordinate& o = nb.offset(i);
    off[i] = static_cast<int>(o.x) + static_cast<int>(o.y) * width + static_cast<int>(o.z) * area;
  }

  return off;
}

// Heap is 1-based with a preserved element at index 0 (value unused).
// checked[v] meanings match legacy:
//  0  : unseen/unqueued
//  1  : finalized
//  >1 : in heap, position = checked[v] - 1
[[nodiscard]] int heapSmallChildI(const std::vector<int>& heap, int parentIndex, const std::vector<double>& value)
{
  const int left = parentIndex * 2;
  if (left >= static_cast<int>(heap.size())) {
    return 0;
  }
  int idx = left;
  const int right = left + 1;
  if (right < static_cast<int>(heap.size())) {
    if (value[static_cast<size_t>(heap[idx])] > value[static_cast<size_t>(heap[right])]) {
      idx = right;
    }
  }
  return idx;
}

void heapSwap(std::vector<int>& heap, int a, int b, std::vector<int>& checked)
{
  std::swap(heap[static_cast<size_t>(a)], heap[static_cast<size_t>(b)]);
  checked[static_cast<size_t>(heap[static_cast<size_t>(a)])] = a + 1;
  checked[static_cast<size_t>(heap[static_cast<size_t>(b)])] = b + 1;
}

void heapAddI(std::vector<int>& heap, int item, const std::vector<double>& value, std::vector<int>& checked)
{
  if (heap.empty()) {
    heap.push_back(0);
  }

  heap.push_back(item);
  checked[static_cast<size_t>(item)] = static_cast<int>(heap.size());

  int childIndex = checked[static_cast<size_t>(item)] - 1;
  int parentIndex = childIndex / 2;
  while (parentIndex > 0) {
    if (value[static_cast<size_t>(heap[static_cast<size_t>(childIndex)])] <
        value[static_cast<size_t>(heap[static_cast<size_t>(parentIndex)])]) {
      heapSwap(heap, childIndex, parentIndex, checked);
      childIndex = parentIndex;
      parentIndex = childIndex / 2;
    } else {
      break;
    }
  }
}

void heapUpdateI(std::vector<int>& heap, int item, const std::vector<double>& value, std::vector<int>& checked)
{
  const int childIndex = checked[static_cast<size_t>(item)] - 1;
  int current = childIndex;
  int parentIndex = current / 2;
  while (parentIndex > 0) {
    if (value[static_cast<size_t>(heap[static_cast<size_t>(current)])] <
        value[static_cast<size_t>(heap[static_cast<size_t>(parentIndex)])]) {
      heapSwap(heap, current, parentIndex, checked);
      current = parentIndex;
      parentIndex = current / 2;
    } else {
      break;
    }
  }
}

[[nodiscard]] int heapRemoveI(std::vector<int>& heap, const std::vector<double>& value, std::vector<int>& checked)
{
  CHECK(heap.size() > 1);
  const int root = heap[1];
  checked[static_cast<size_t>(root)] = 0;

  heap[1] = heap.back();
  heap.pop_back();

  if (heap.size() > 1) {
    checked[static_cast<size_t>(heap[1])] = 2;
  }

  int parentIndex = 1;
  int childIndex = heapSmallChildI(heap, parentIndex, value);
  while (childIndex > 0) {
    if (value[static_cast<size_t>(heap[static_cast<size_t>(parentIndex)])] >
        value[static_cast<size_t>(heap[static_cast<size_t>(childIndex)])]) {
      heapSwap(heap, parentIndex, childIndex, checked);
      parentIndex = childIndex;
      childIndex = heapSmallChildI(heap, parentIndex, value);
    } else {
      break;
    }
  }

  return root;
}

[[nodiscard]] int extractMin(std::vector<double>& dist, std::vector<int>& checked, std::vector<int>& heap)
{
  if (heap.size() <= 1) {
    return -1;
  }

  const int minIdx = heapRemoveI(heap, dist, checked);
  const double minVal = dist[static_cast<size_t>(minIdx)];

  if (minVal == std::numeric_limits<double>::infinity()) {
    return -1;
  }

  checked[static_cast<size_t>(minIdx)] = 1;
  return minIdx;
}

void updateWaiting(const ZImg& stack,
                   size_t r,
                   size_t nbrIndex,
                   double weight,
                   std::vector<int>& heap,
                   SpGrowWorkspace& sgw)
{
  if ((sgw.checked[nbrIndex] != 1) && (sgw.flag[nbrIndex] != SP_GROW_BARRIER)) {
    double tmpdist = sgw.dist[r];
    const double eucdist = weight;

    if (sgw.spOption == 0 && sgw.weightFunc != nullptr) {
      sgw.argv[0] = weight;
      sgw.argv[1] = stackArrayValue(stack, r);
      sgw.argv[2] = stackArrayValue(stack, nbrIndex);
      weight = sgw.weightFunc(sgw.argv.data());
    }

    if (sgw.spOption == 1) {
      tmpdist = std::max(tmpdist, -stackArrayValue(stack, r));
    } else {
      tmpdist += weight;
    }

    if (tmpdist < sgw.dist[nbrIndex]) {
      sgw.dist[nbrIndex] = tmpdist;
      sgw.path[nbrIndex] = static_cast<int>(r);
      if (!sgw.length.empty()) {
        sgw.length[nbrIndex] = sgw.length[r] + eucdist;
      }
    }

    if (sgw.spOption == 1) {
      if (tmpdist == sgw.dist[nbrIndex]) {
        if (sgw.path[nbrIndex] >= 0) {
          const double v1 = stackArrayValue(stack, static_cast<size_t>(sgw.path[nbrIndex]));
          const double v2 = stackArrayValue(stack, r);
          if (v2 > v1) {
            sgw.path[nbrIndex] = static_cast<int>(r);
          }
        }
      }
    }

    if (sgw.checked[nbrIndex] > 1) {
      heapUpdateI(heap, static_cast<int>(nbrIndex), sgw.dist, sgw.checked);
    } else if (sgw.checked[nbrIndex] <= 0) {
      heapAddI(heap, static_cast<int>(nbrIndex), sgw.dist, sgw.checked);
    }
  }
}

[[nodiscard]] int stackSpGrowInternalLegacyLike(const ZImg& stack, SpGrowWorkspace& sgw)
{
  const size_t nvoxel = stack.voxelNumber();
  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());

  sgw.width = width;
  sgw.height = height;
  sgw.depth = depth;
  sgw.size = nvoxel;

  sgw.dist.assign(nvoxel, std::numeric_limits<double>::infinity());
  sgw.path.assign(nvoxel, -1);
  sgw.checked.assign(nvoxel, 0);
  sgw.flag.assign(nvoxel, 0);

  if (sgw.lengthBufferEnabled) {
    sgw.length.assign(nvoxel, 0.0);
  } else {
    sgw.length.clear();
  }

  CHECK(!sgw.mask.empty()) << "stackSpGrow expects sgw.mask to be set";
  CHECK(sgw.mask.size() == nvoxel);

  sgw.flag = sgw.mask;

  // Recruit sources from mask.
  for (size_t i = 0; i < nvoxel; ++i) {
    if (sgw.mask[i] == SP_GROW_SOURCE) {
      if (sgw.spOption == 1) {
        sgw.dist[i] = -stackArrayValue(stack, i);
      } else {
        sgw.dist[i] = 0.0;
      }
      sgw.checked[i] = 1;
    }
  }

  std::vector<double> nbrDist = neighborDistR(sgw.conn, sgw.resolution);
  std::vector<int> nbrOff = neighborOffset(sgw.conn, width, height);
  const ZNeighborhood& nb = neighborhoodLegacyOrder(sgw.conn);

  auto inBound = [width, height, depth](int x, int y, int z) -> bool {
    return x >= 0 && y >= 0 && z >= 0 && x < width && y < height && z < depth;
  };

  auto indexToCoord = [width, height](size_t idx) -> std::array<int, 3> {
    const size_t area = static_cast<size_t>(width) * static_cast<size_t>(height);
    const int z = static_cast<int>(idx / area);
    const size_t rem = idx - static_cast<size_t>(z) * area;
    const int y = static_cast<int>(rem / static_cast<size_t>(width));
    const int x = static_cast<int>(rem - static_cast<size_t>(y) * static_cast<size_t>(width));
    return {x, y, z};
  };

  // Heap of frontier voxels.
  std::vector<int> heap;
  heap.reserve(1024);
  heap.push_back(0);

  // Initialize heap with neighbors of sources.
  for (size_t r = 0; r < nvoxel; ++r) {
    if (sgw.flag[r] != SP_GROW_SOURCE) {
      continue;
    }

    const auto [x, y, z] = indexToCoord(r);
    for (size_t j = 0; j < nb.size(); ++j) {
      const ZVoxelCoordinate& o = nb.offset(j);
      const int nx = x + o.x;
      const int ny = y + o.y;
      const int nz = z + o.z;
      if (!inBound(nx, ny, nz)) {
        continue;
      }
      const size_t nbrIndex = static_cast<size_t>(static_cast<int>(r) + nbrOff[j]);
      if (sgw.checked[nbrIndex] != 1) {
        updateWaiting(stack, r, nbrIndex, nbrDist[j], heap, sgw);
      }
    }
  }

  int lastR = -1;
  while (true) {
    const int r = extractMin(sgw.dist, sgw.checked, heap);
    if (r < 0) {
      break;
    }

    lastR = r;

    if (sgw.flag[static_cast<size_t>(r)] == 0) {
      const auto [x, y, z] = indexToCoord(static_cast<size_t>(r));
      for (size_t j = 0; j < nb.size(); ++j) {
        const ZVoxelCoordinate& o = nb.offset(j);
        const int nx = x + o.x;
        const int ny = y + o.y;
        const int nz = z + o.z;
        if (!inBound(nx, ny, nz)) {
          continue;
        }
        const size_t nbrIndex = static_cast<size_t>(r + nbrOff[j]);
        if (sgw.checked[nbrIndex] != 1) {
          updateWaiting(stack, static_cast<size_t>(r), nbrIndex, nbrDist[j], heap, sgw);
        }
      }
    } else if (sgw.flag[static_cast<size_t>(r)] == SP_GROW_TARGET) {
      break;
    } else if (sgw.flag[static_cast<size_t>(r)] == SP_GROW_CONDUCTOR) {
      // Super conductor region handling is not needed for current neutube usage;
      // preserve legacy semantics elsewhere if/when it becomes required.
    }
  }

  return lastR;
}

} // namespace

void stackSpGrow(const ZImg& stack, SpGrowWorkspace* sgw)
{
  CHECK(sgw != nullptr);
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  (void)stackSpGrowInternalLegacyLike(stack, *sgw);
}

std::vector<int64_t> stackSpGrowPathLegacyLike(const ZImg& stack, SpGrowWorkspace* sgw)
{
  CHECK(sgw != nullptr);
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const int lastR = stackSpGrowInternalLegacyLike(stack, *sgw);
  if (lastR < 0) {
    return {};
  }

  sgw->value = sgw->dist[static_cast<size_t>(lastR)];

  std::vector<int64_t> path;
  int idx = lastR;
  while (idx >= 0) {
    path.push_back(static_cast<int64_t>(idx));
    const int next = sgw->path[static_cast<size_t>(idx)];
    CHECK(next != idx);
    idx = next;
  }

  std::reverse(path.begin(), path.end());
  return path;
}

} // namespace nim::neutube
