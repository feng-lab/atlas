#include "zneutubeimglocmax.h"

#include "zneutubeneighborhood.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nim {

namespace {

constexpr int kStackOrientationDf = 13;

// 27 types of location (8 vertices, 12 edges, 6 faces, 1 internal).
// Ported from src/neurolabi/c/private/tz_stack_lib.c.
constexpr std::array<std::array<int, kStackOrientationDf>, 27> kScanMask = {
  {
   {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}, // 0
    {{1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0}}, // 1
    {{0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0}}, // 2
    {{1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}}, // 3
    {{0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1}}, // 4
    {{1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // 5
    {{0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}}, // 6
    {{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // 7
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // 8
    {{1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0}}, // 9 (1|2)
    {{1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1}}, // 10 (3|4)
    {{1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}}, // 11 (5|6)
    {{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // 12 (7|8)
    {{1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0}}, // 13 (1|3)
    {{0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1}}, // 14 (2|4)
    {{1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // 15 (5|7)
    {{0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}}, // 16 (6|8)
    {{1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0}}, // 17 (1|5)
    {{0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0}}, // 18 (2|6)
    {{1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}}, // 19 (3|7)
    {{0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1}}, // 20 (4|8)
    {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}, // 21 (1|2|3|4)
    {{1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}}, // 22 (5|6|7|8)
    {{1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0}}, // 23 (1|3|5|7)
    {{0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1}}, // 24 (2|4|6|8)
    {{1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0}}, // 25 (1|2|5|6)
    {{1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1}}, // 26 (3|4|7|8)
  }
};

// for depth 1
constexpr std::array<int, kStackOrientationDf> kScanMaskDepth = {
  {1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}
};

// for height 1
constexpr std::array<int, kStackOrientationDf> kScanMaskHeight = {
  {1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0}
};

// for width 1
constexpr std::array<int, kStackOrientationDf> kScanMaskWidth = {
  {0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}
};

void initScanArray(int width, int height, int* neighbor)
{
  const int planeOffset = width * height;
  neighbor[0] = 1; // x direction
  neighbor[1] = width; // y direction
  neighbor[2] = planeOffset; // z direction
  neighbor[3] = width + 1; // x-y diagonal
  neighbor[4] = width - 1; // x-y counterdiagonal
  neighbor[5] = planeOffset + 1; // x-z diagonal
  neighbor[6] = planeOffset - 1; // x-z counterdiagonal
  neighbor[7] = planeOffset + width; // y-z diagonal
  neighbor[8] = planeOffset - width; // y-z counterdiagonal
  neighbor[9] = planeOffset + width + 1; // x-y-z diagonal
  neighbor[10] = planeOffset + width - 1; // x-y-z diagonal -x
  neighbor[11] = planeOffset - width + 1; // x-y-z diagonal -y
  neighbor[12] = planeOffset - width - 1; // x-y-z diagonal -x -y
}

void copyScanArrayMask(int id, int* mask)
{
  CHECK(mask != nullptr);
  CHECK(id >= 0 && id < static_cast<int>(kScanMask.size()));
  std::memcpy(mask, kScanMask[static_cast<size_t>(id)].data(), sizeof(int) * kStackOrientationDf);
}

void applyScanMaskDepth(int* mask)
{
  for (int i = 0; i < kStackOrientationDf; ++i) {
    mask[i] = mask[i] && kScanMaskDepth[static_cast<size_t>(i)];
  }
}

void applyScanMaskHeight(int* mask)
{
  for (int i = 0; i < kStackOrientationDf; ++i) {
    mask[i] = mask[i] && kScanMaskHeight[static_cast<size_t>(i)];
  }
}

void applyScanMaskWidth(int* mask)
{
  for (int i = 0; i < kStackOrientationDf; ++i) {
    mask[i] = mask[i] && kScanMaskWidth[static_cast<size_t>(i)];
  }
}

int boundaryOffset(int width, int height, int depth, int id, int* offsets)
{
  CHECK(offsets != nullptr);

  int n = 0;
  const int area = width * height;
  const int volume = area * depth;

  const int cwidth = width - 1;
  const int carea = area - width;
  const int cvolume = volume - area;

  int start = 0;

  switch (id) {
    case 1:
      offsets[0] = 0;
      n = 1;
      break;
    case 2:
      offsets[0] = cwidth;
      n = 1;
      break;
    case 3:
      offsets[0] = carea;
      n = 1;
      break;
    case 4:
      offsets[0] = carea + cwidth;
      n = 1;
      break;
    case 5:
      offsets[0] = cvolume;
      n = 1;
      break;
    case 6:
      offsets[0] = cvolume + cwidth;
      n = 1;
      break;
    case 7:
      offsets[0] = cvolume + carea;
      n = 1;
      break;
    case 8:
      offsets[0] = volume - 1;
      n = 1;
      break;
    case 9:
      for (int i = 1; i < cwidth; ++i) {
        offsets[n++] = i;
      }
      break;
    case 10:
      start = carea;
      for (int i = 1; i < cwidth; ++i) {
        offsets[n++] = start + i;
      }
      break;
    case 11:
      start = cvolume;
      for (int i = 1; i < cwidth; ++i) {
        offsets[n++] = start + i;
      }
      break;
    case 12:
      start = cvolume + carea;
      for (int i = 1; i < cwidth; ++i) {
        offsets[n++] = start + i;
      }
      break;
    case 13:
      for (int i = width; i < carea; i += width) {
        offsets[n++] = i;
      }
      break;
    case 14:
      start = cwidth;
      for (int i = width; i < carea; i += width) {
        offsets[n++] = start + i;
      }
      break;
    case 15:
      start = cvolume;
      for (int i = width; i < carea; i += width) {
        offsets[n++] = start + i;
      }
      break;
    case 16:
      start = cvolume + cwidth;
      for (int i = width; i < carea; i += width) {
        offsets[n++] = start + i;
      }
      break;
    case 17:
      for (int i = area; i < cvolume; i += area) {
        offsets[n++] = i;
      }
      break;
    case 18:
      start = cwidth;
      for (int i = area; i < cvolume; i += area) {
        offsets[n++] = start + i;
      }
      break;
    case 19:
      start = carea;
      for (int i = area; i < cvolume; i += area) {
        offsets[n++] = start + i;
      }
      break;
    case 20:
      start = carea + cwidth;
      for (int i = area; i < cvolume; i += area) {
        offsets[n++] = start + i;
      }
      break;
    case 21:
      for (int j = width; j < carea; j += width) {
        for (int i = 1; i < cwidth; ++i) {
          offsets[n++] = i + j;
        }
      }
      break;
    case 22:
      start = cvolume;
      for (int j = width; j < carea; j += width) {
        for (int i = 1; i < cwidth; ++i) {
          offsets[n++] = start + i + j;
        }
      }
      break;
    case 23:
      for (int j = area; j < cvolume; j += area) {
        for (int i = width; i < carea; i += width) {
          offsets[n++] = i + j;
        }
      }
      break;
    case 24:
      start = cwidth;
      for (int j = area; j < cvolume; j += area) {
        for (int i = width; i < carea; i += width) {
          offsets[n++] = start + i + j;
        }
      }
      break;
    case 25:
      for (int j = area; j < cvolume; j += area) {
        for (int i = 1; i < cwidth; ++i) {
          offsets[n++] = i + j;
        }
      }
      break;
    case 26:
      start = carea;
      for (int j = area; j < cvolume; j += area) {
        for (int i = 1; i < cwidth; ++i) {
          offsets[n++] = start + i + j;
        }
      }
      break;
    default:
      break;
  }

  return n;
}

template<typename TScalar>
void arrayCmp(const TScalar* array,
              uint8_t* out,
              size_t offset,
              int c,
              int* nboffset,
              const int* neighbor,
              StackLocmaxOptionLegacyLike option)
{
  *nboffset = static_cast<int>(offset) + neighbor[c];
  const size_t nb = static_cast<size_t>(*nboffset);

  if (array[nb] > static_cast<TScalar>(0)) {
    if (array[offset] < array[nb]) {
      out[offset] = 0;
    } else if (array[offset] > array[nb]) {
      out[nb] = 0;
    } else {
      switch (option) {
        case StackLocmaxOptionLegacyLike::Center:
          out[nb] = 0;
          break;
        case StackLocmaxOptionLegacyLike::Neighbor:
          out[offset] = 0;
          break;
        case StackLocmaxOptionLegacyLike::NonFlat:
          out[offset] = 0;
          out[nb] = 0;
          break;
        case StackLocmaxOptionLegacyLike::Alter1:
          if (out[offset] == 1 && out[nb] == 1) {
            out[offset] = 0;
          }
          break;
        case StackLocmaxOptionLegacyLike::Alter2:
          if (out[offset] == 1 && out[nb] == 1) {
            out[nb] = 0;
          }
          break;
        case StackLocmaxOptionLegacyLike::Single:
          if (out[offset] == 1 && out[nb] == 1) {
            out[nb] = 0;
          } else {
            out[offset] = 0;
            out[nb] = 0;
          }
          break;
        default:
          break;
      }
    }
  } else {
    out[nb] = 0;
  }
}

template<typename TScalar>
void stackLocalMaxMaskImpl(const ZImg& stack, uint8_t* out, StackLocmaxOptionLegacyLike option)
{
  CHECK(out != nullptr);

  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());
  CHECK(width > 0 && height > 0 && depth > 0);

  const int area = width * height;
  const size_t voxelNumber = stack.voxelNumber();

  // boundary_size selection matches legacy.
  int boundarySize = area;
  if ((width < depth) || (height < depth)) {
    if (height < width) {
      boundarySize = width * depth;
    } else {
      boundarySize = height * depth;
    }
  }

  std::vector<int> boundary(static_cast<size_t>(boundarySize));
  std::array<int, kStackOrientationDf> neighbor{};
  std::array<int, kStackOrientationDf> mask{};

  initScanArray(width, height, neighbor.data());

  const auto* array = stack.timeData<TScalar>(0);

  int nboffset = 0;

  // Process boundaries.
  for (int id = 1; id <= 26; ++id) {
    copyScanArrayMask(id, mask.data());
    if (depth == 1) {
      applyScanMaskDepth(mask.data());
    }
    if (height == 1) {
      applyScanMaskHeight(mask.data());
    }
    if (width == 1) {
      applyScanMaskWidth(mask.data());
    }

    const int nbr = boundaryOffset(width, height, depth, id, boundary.data());
    for (int i = 0; i < nbr; ++i) {
      const size_t offset = static_cast<size_t>(boundary[static_cast<size_t>(i)]);
      if (array[offset] == static_cast<TScalar>(0)) {
        out[offset] = 0;
      }
      for (int c = 0; c < kStackOrientationDf; ++c) {
        if (mask[static_cast<size_t>(c)] == 1) {
          arrayCmp(array, out, offset, c, &nboffset, neighbor.data(), option);
        }
      }
    }
  }

  // Internal voxels.
  //
  // Legacy only has "internal" voxels when all 3 dims are at least 3. For 2D images (depth==1),
  // all voxels are considered boundary voxels and are covered by the boundary passes above.
  if (width > 2 && height > 2 && depth > 2) {
    size_t offset = static_cast<size_t>(area + width + 1);
    for (int k = 1; k < depth - 1; ++k) {
      for (int j = 1; j < height - 1; ++j) {
        for (int i = 1; i < width - 1; ++i) {
          if (array[offset] == static_cast<TScalar>(0)) {
            out[offset] = 0;
          }
          for (int c = 0; c < kStackOrientationDf; ++c) {
            arrayCmp(array, out, offset, c, &nboffset, neighbor.data(), option);
          }
          ++offset;
        }
        offset += 2;
      }
      offset += static_cast<size_t>(width * 2);
    }

    CHECK(offset <= voxelNumber);
  }
}

template<typename TScalar>
void stackLocmaxRegionMaskImpl(const ZImg& stack, uint8_t* out, int connectivity)
{
  CHECK(out != nullptr);

  const int width = static_cast<int>(stack.width());
  const int height = static_cast<int>(stack.height());
  const int depth = static_cast<int>(stack.depth());
  CHECK(width > 0 && height > 0 && depth > 0);

  const int cwidth = width - 1;
  const int cheight = height - 1;
  const int cdepth = depth - 1;

  const size_t voxelNumber = stack.voxelNumber();
  const size_t planeSize = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t widthS = static_cast<size_t>(width);

  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);
  CHECK(nb.size() == static_cast<size_t>(connectivity));

  const auto* array = stack.timeData<TScalar>(0);

  // Initialize queue with non-max voxels.
  //
  // This can grow large for dense volumes. Use a vector-backed FIFO queue to reduce
  // allocator/iterator overhead compared to std::deque in this hot path.
  std::vector<size_t> q;
  q.reserve(1024);
  size_t qHead = 0;

  size_t offset = 0;
  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x, ++offset) {
        if (array[offset] <= static_cast<TScalar>(0)) {
          out[offset] = 0;
          continue;
        }

        for (size_t i = 0; i < nb.size(); ++i) {
          const ZVoxelCoordinate& o = nb.offset(i);
          const int nx = x + o.x;
          const int ny = y + o.y;
          const int nz = z + o.z;
          if (nx < 0 || ny < 0 || nz < 0 || nx > cwidth || ny > cheight || nz > cdepth) {
            continue;
          }

          const size_t nidx =
            static_cast<size_t>(nx) + static_cast<size_t>(ny) * widthS + static_cast<size_t>(nz) * planeSize;
          if (array[offset] < array[nidx]) {
            q.push_back(offset);
            out[offset] = 0;
            break;
          }
        }
      }
    }
  }

  // Propagate removals (non-increasing paths from non-max voxels).
  while (qHead < q.size()) {
    const size_t c = q[qHead];
    ++qHead;

    const int z = static_cast<int>(c / planeSize);
    const size_t rem = c - static_cast<size_t>(z) * planeSize;
    const int y = static_cast<int>(rem / widthS);
    const int x = static_cast<int>(rem - static_cast<size_t>(y) * widthS);

    for (size_t i = 0; i < nb.size(); ++i) {
      const ZVoxelCoordinate& o = nb.offset(i);
      const int nx = x + o.x;
      const int ny = y + o.y;
      const int nz = z + o.z;
      if (nx < 0 || ny < 0 || nz < 0 || nx > cwidth || ny > cheight || nz > cdepth) {
        continue;
      }
      const size_t nidx =
        static_cast<size_t>(nx) + static_cast<size_t>(ny) * widthS + static_cast<size_t>(nz) * planeSize;
      if (out[nidx] == 1) {
        if (array[nidx] <= array[c]) {
          out[nidx] = 0;
          q.push_back(nidx);
        }
      }
    }
  }

  CHECK(voxelNumber == offset);
}

[[nodiscard]] ZImg makeUint8VolumeLike(const ZImg& stack)
{
  ZImgInfo info = stack.info();
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  ZImg out(info);
  return out;
}

} // namespace

ZImg stackLocalMaxMaskLegacyLike(const ZImg& stack, StackLocmaxOptionLegacyLike option)
{
  if (stack.isEmpty()) {
    return {};
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  ZImg out = makeUint8VolumeLike(stack);
  out.fill(1);

  auto* outData = out.timeData<uint8_t>(0);

  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    stackLocalMaxMaskImpl<TVoxel>(stack, outData, option);
  });

  return out;
}

ZImg stackLocmaxRegionMaskLegacyLike(const ZImg& stack, int connectivity)
{
  if (stack.isEmpty()) {
    return {};
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (connectivity != 4 && connectivity != 8 && connectivity != 6 && connectivity != 10 && connectivity != 18 &&
      connectivity != 26) {
    throw ZException(fmt::format("stackLocmaxRegionMaskLegacyLike: unsupported connectivity {}", connectivity));
  }

  ZImg out = makeUint8VolumeLike(stack);
  out.fill(1);

  auto* outData = out.timeData<uint8_t>(0);

  imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() {
    stackLocmaxRegionMaskImpl<TVoxel>(stack, outData, connectivity);
  });

  return out;
}

} // namespace nim
