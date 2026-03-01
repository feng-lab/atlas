#include "zimgbwthinlegacy.h"

#include "zlog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nim {

namespace {

// Tables from tz_stack_bwmorph.c (Lee et al. 3D thinning).
static constexpr std::array<int, 256> kEulerTable = {
  0, 1,  0, -1, 0, -1, 0, 1, 0, -3, 0, -1, 0, -1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, -3, 0, -1, 0, 3,  0, 1, 0, 1,  0, -1, 0, 3,  0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, -3, 0, 3,  0, -1, 0, 1, 0, 1,  0, 3,  0, -1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, 1,  0, 3,  0, 3,  0, 1, 0, 5,  0, 3,  0, 3,  0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, -7, 0, -1, 0, -1, 0, 1, 0, -3, 0, -1, 0, -1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, -3, 0, -1, 0, 3,  0, 1, 0, 1,  0, -1, 0, 3,  0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, -3, 0, 3,  0, -1, 0, 1, 0, 1,  0, 3,  0, -1, 0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1,
  0, 1,  0, 3,  0, 3,  0, 1, 0, 5,  0, 3,  0, 3,  0, 1, 0, -1, 0, 1, 0, 1, 0, -1, 0, 3, 0, 1, 0, 1, 0, -1};

static constexpr int kBwthinOctant[8][7] = {
  {23, 24, 14, 15, 20, 21, 12},
  {25, 22, 16, 13, 24, 21, 15},
  {17, 20, 9,  12, 18, 21, 10},
  {19, 22, 18, 21, 11, 13, 10},
  {6,  14, 7,  15, 3,  12, 4 },
  {8,  7,  16, 15, 5,  4,  13},
  {0,  9,  3,  12, 1,  10, 4 },
  {2,  1,  11, 10, 5,  4,  13},
};

static constexpr std::array<unsigned char, 7> kOctantCode = {128, 64, 32, 16, 8, 4, 2};

static constexpr std::array<int, 26> kOctantIndex = {1, 1, 2, 1, 1, 2, 3, 3, 4, 1, 1, 2, 1,
                                                     2, 3, 3, 4, 5, 5, 6, 5, 5, 6, 7, 7, 8};

[[nodiscard]] bool isEulerInvariant(const int* neighbor)
{
  int eulerNumber = 0;
  for (int i = 0; i < 8; ++i) {
    unsigned char eulerCode = 1;
    for (int j = 0; j < 7; ++j) {
      if (neighbor[kBwthinOctant[i][j]] > 0) {
        eulerCode |= kOctantCode[static_cast<size_t>(j)];
      }
    }
    eulerNumber += kEulerTable[static_cast<size_t>(eulerCode)];
  }
  return eulerNumber == 0;
}

void octreeLabel(int index, int label, int* neighborLabel)
{
  switch (index) {
    case 1:
      if (neighborLabel[0] == 1) {
        neighborLabel[0] = label;
      }
      if (neighborLabel[1] == 1) {
        neighborLabel[1] = label;
        octreeLabel(2, label, neighborLabel);
      }
      if (neighborLabel[3] == 1) {
        neighborLabel[3] = label;
        octreeLabel(3, label, neighborLabel);
      }
      if (neighborLabel[4] == 1) {
        neighborLabel[4] = label;
        octreeLabel(2, label, neighborLabel);
        octreeLabel(3, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[9] == 1) {
        neighborLabel[9] = label;
        octreeLabel(5, label, neighborLabel);
      }
      if (neighborLabel[10] == 1) {
        neighborLabel[10] = label;
        octreeLabel(2, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[12] == 1) {
        neighborLabel[12] = label;
        octreeLabel(3, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
      }
      break;
    case 2:
      if (neighborLabel[1] == 1) {
        neighborLabel[1] = label;
        octreeLabel(1, label, neighborLabel);
      }
      if (neighborLabel[4] == 1) {
        neighborLabel[4] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(3, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[10] == 1) {
        neighborLabel[10] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[2] == 1) {
        neighborLabel[2] = label;
      }
      if (neighborLabel[5] == 1) {
        neighborLabel[5] = label;
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[11] == 1) {
        neighborLabel[11] = label;
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[13] == 1) {
        neighborLabel[13] = label;
        octreeLabel(4, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 3:
      if (neighborLabel[3] == 1) {
        neighborLabel[3] = label;
        octreeLabel(1, label, neighborLabel);
      }
      if (neighborLabel[4] == 1) {
        neighborLabel[4] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(2, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[12] == 1) {
        neighborLabel[12] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[6] == 1) {
        neighborLabel[6] = label;
      }
      if (neighborLabel[7] == 1) {
        neighborLabel[7] = label;
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[14] == 1) {
        neighborLabel[14] = label;
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[15] == 1) {
        neighborLabel[15] = label;
        octreeLabel(4, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 4:
      if (neighborLabel[4] == 1) {
        neighborLabel[4] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(2, label, neighborLabel);
        octreeLabel(3, label, neighborLabel);
      }
      if (neighborLabel[5] == 1) {
        neighborLabel[5] = label;
        octreeLabel(2, label, neighborLabel);
      }
      if (neighborLabel[13] == 1) {
        neighborLabel[13] = label;
        octreeLabel(2, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[7] == 1) {
        neighborLabel[7] = label;
        octreeLabel(3, label, neighborLabel);
      }
      if (neighborLabel[15] == 1) {
        neighborLabel[15] = label;
        octreeLabel(3, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[8] == 1) {
        neighborLabel[8] = label;
      }
      if (neighborLabel[16] == 1) {
        neighborLabel[16] = label;
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 5:
      if (neighborLabel[9] == 1) {
        neighborLabel[9] = label;
        octreeLabel(1, label, neighborLabel);
      }
      if (neighborLabel[10] == 1) {
        neighborLabel[10] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(2, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[12] == 1) {
        neighborLabel[12] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(3, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[17] == 1) {
        neighborLabel[17] = label;
      }
      if (neighborLabel[18] == 1) {
        neighborLabel[18] = label;
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[20] == 1) {
        neighborLabel[20] = label;
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[21] == 1) {
        neighborLabel[21] = label;
        octreeLabel(6, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 6:
      if (neighborLabel[10] == 1) {
        neighborLabel[10] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(2, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
      }
      if (neighborLabel[11] == 1) {
        neighborLabel[11] = label;
        octreeLabel(2, label, neighborLabel);
      }
      if (neighborLabel[13] == 1) {
        neighborLabel[13] = label;
        octreeLabel(2, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[18] == 1) {
        neighborLabel[18] = label;
        octreeLabel(5, label, neighborLabel);
      }
      if (neighborLabel[21] == 1) {
        neighborLabel[21] = label;
        octreeLabel(5, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[19] == 1) {
        neighborLabel[19] = label;
      }
      if (neighborLabel[22] == 1) {
        neighborLabel[22] = label;
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 7:
      if (neighborLabel[12] == 1) {
        neighborLabel[12] = label;
        octreeLabel(1, label, neighborLabel);
        octreeLabel(3, label, neighborLabel);
        octreeLabel(5, label, neighborLabel);
      }
      if (neighborLabel[14] == 1) {
        neighborLabel[14] = label;
        octreeLabel(3, label, neighborLabel);
      }
      if (neighborLabel[15] == 1) {
        neighborLabel[15] = label;
        octreeLabel(3, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[20] == 1) {
        neighborLabel[20] = label;
        octreeLabel(5, label, neighborLabel);
      }
      if (neighborLabel[21] == 1) {
        neighborLabel[21] = label;
        octreeLabel(5, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
        octreeLabel(8, label, neighborLabel);
      }
      if (neighborLabel[23] == 1) {
        neighborLabel[23] = label;
      }
      if (neighborLabel[24] == 1) {
        neighborLabel[24] = label;
        octreeLabel(8, label, neighborLabel);
      }
      break;
    case 8:
      if (neighborLabel[13] == 1) {
        neighborLabel[13] = label;
        octreeLabel(2, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[15] == 1) {
        neighborLabel[15] = label;
        octreeLabel(3, label, neighborLabel);
        octreeLabel(4, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[16] == 1) {
        neighborLabel[16] = label;
        octreeLabel(4, label, neighborLabel);
      }
      if (neighborLabel[21] == 1) {
        neighborLabel[21] = label;
        octreeLabel(5, label, neighborLabel);
        octreeLabel(6, label, neighborLabel);
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[22] == 1) {
        neighborLabel[22] = label;
        octreeLabel(6, label, neighborLabel);
      }
      if (neighborLabel[24] == 1) {
        neighborLabel[24] = label;
        octreeLabel(7, label, neighborLabel);
      }
      if (neighborLabel[25] == 1) {
        neighborLabel[25] = label;
      }
      break;
    default:
      break;
  }
}

[[nodiscard]] bool isSimpleConnectPoint(const int* neighbor)
{
  int label = 2;
  int neighborLabel[26];
  for (int i = 0; i < 26; ++i) {
    neighborLabel[i] = neighbor[i] > 0;
  }

  for (int i = 0; i < 26; ++i) {
    if (neighborLabel[i] == 1) {
      octreeLabel(kOctantIndex[static_cast<size_t>(i)], label, neighborLabel);
      ++label;
      if (label >= 4) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::array<std::ptrdiff_t, 26> neighborOffsets(int width, std::ptrdiff_t area)
{
  // Neighbor order matches `stack_bwthin_neighbor_value`:
  // - z-1 plane (3x3), then z plane (8 neighbors), then z+1 plane (3x3).
  const std::ptrdiff_t w = static_cast<std::ptrdiff_t>(width);
  return {-1 - w - area, 0 - w - area, 1 - w - area,  -1 - area,     0 - area,     1 - area,     -1 + w - area,
          0 + w - area,  1 + w - area, -1 - w,        0 - w,         1 - w,        -1,           1,
          -1 + w,        0 + w,        1 + w,         -1 - w + area, 0 - w + area, 1 - w + area, -1 + area,
          0 + area,      1 + area,     -1 + w + area, 0 + w + area,  1 + w + area};
}

inline void
neighborValues(const uint8_t* data, size_t index, const std::array<std::ptrdiff_t, 26>& offsets, int* outNeighbor)
{
  const std::ptrdiff_t base = static_cast<std::ptrdiff_t>(index);
  for (size_t i = 0; i < 26; ++i) {
    outNeighbor[i] = data[static_cast<size_t>(base + offsets[i])];
  }
}

[[nodiscard]] bool
isCandidateVoxel(const uint8_t* data, size_t index, int border, const std::array<std::ptrdiff_t, 26>& offsets)
{
  if (data[index] == 0) {
    return false;
  }

  int neighbor[26];
  neighborValues(data, index, offsets, neighbor);

  if (!(border == 5 && neighbor[4] == 0) && !(border == 0 && neighbor[10] == 0) &&
      !(border == 3 && neighbor[12] == 0) && !(border == 2 && neighbor[13] == 0) &&
      !(border == 1 && neighbor[15] == 0) && !(border == 4 && neighbor[21] == 0)) {
    return false;
  }

  int neighborOn = 0;
  for (int i = 0; i < 26; ++i) {
    if (neighbor[i] > 0) {
      ++neighborOn;
    }
  }

  // NORMAL_THINNING: endpoints (<=1 neighbor) are not candidates.
  if (neighborOn <= 1) {
    return false;
  }

  if (!isEulerInvariant(neighbor)) {
    return false;
  }

  if (!isSimpleConnectPoint(neighbor)) {
    return false;
  }

  return true;
}

} // namespace

ZImg bwthinLegacyLike(const ZImg& binary)
{
  if (binary.isEmpty()) {
    return {};
  }
  if (binary.voxelFormat() != VoxelFormat::Unsigned || binary.voxelByteNumber() != sizeof(uint8_t)) {
    LOG(WARNING) << "bwthinLegacyLike: expected unsigned 8-bit input; got " << binary.info();
    return {};
  }

  const int w = static_cast<int>(binary.width());
  const int h = static_cast<int>(binary.height());
  const int d = static_cast<int>(binary.depth());

  ZImgInfo outInfo = binary.info();
  outInfo.setVoxelFormat<uint8_t>();
  outInfo.createDefaultDescriptions();

  // Stack_Bwpeel pads by 1 voxel on all sides.
  const int pw = w + 2;
  const int ph = h + 2;
  const int pd = d + 2;
  const std::ptrdiff_t area = static_cast<std::ptrdiff_t>(pw) * static_cast<std::ptrdiff_t>(ph);

  const std::array<std::ptrdiff_t, 26> offsets = neighborOffsets(pw, area);

  // Operate per (c,t) independently (legacy Stack is single-channel).
  ZImg out(outInfo);
  out.fill<uint8_t>(0);

  for (size_t t = 0; t < binary.numTimes(); ++t) {
    for (size_t c = 0; c < binary.numChannels(); ++c) {
      std::vector<uint8_t> padded(static_cast<size_t>(pw) * static_cast<size_t>(ph) * static_cast<size_t>(pd), 0);

      // Copy into padded interior at +1 offset.
      for (int z = 0; z < d; ++z) {
        const uint8_t* src = binary.rowData<uint8_t>(0, static_cast<size_t>(z), c, t);
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            const uint8_t v = src[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            const size_t dstIndex = static_cast<size_t>(x + 1) + static_cast<size_t>(y + 1) * static_cast<size_t>(pw) +
                                    static_cast<size_t>(z + 1) * static_cast<size_t>(pw) * static_cast<size_t>(ph);
            padded[dstIndex] = (v > 0) ? 1 : 0;
          }
        }
      }

      size_t foregroundSize = 0;
      for (uint8_t v : padded) {
        if (v > 0) {
          ++foregroundSize;
        }
      }
      std::vector<size_t> checklist;
      checklist.reserve(foregroundSize);

      const int cwidth = pw - 1;
      const int cheight = ph - 1;
      const int cdepth = pd - 1;

      int changed = 6;
      while (changed) {
        changed = 6;
        for (int border = 0; border < 6; ++border) {
          checklist.clear();

          for (int z = 1; z < cdepth; ++z) {
            for (int y = 1; y < cheight; ++y) {
              for (int x = 1; x < cwidth; ++x) {
                const size_t index = static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(pw) +
                                     static_cast<size_t>(z) * static_cast<size_t>(pw) * static_cast<size_t>(ph);
                if (isCandidateVoxel(padded.data(), index, border, offsets)) {
                  checklist.push_back(index);
                }
              }
            }
          }

          bool voxelRemoved = false;
          int neighbor[26];
          for (size_t i = 0; i < checklist.size(); ++i) {
            const size_t index = checklist[i];
            neighborValues(padded.data(), index, offsets, neighbor);

            bool hasOnNeighbor = false;
            for (int n = 0; n < 26; ++n) {
              if (neighbor[n] > 0) {
                hasOnNeighbor = true;
                break;
              }
            }

            if (hasOnNeighbor) {
              if (isSimpleConnectPoint(neighbor)) {
                padded[index] = 0;
                voxelRemoved = true;
              }
            }
          }

          if (!voxelRemoved) {
            --changed;
          }
        }
      }

      // Crop out the +1 padding.
      for (int z = 0; z < d; ++z) {
        uint8_t* dst = out.rowData<uint8_t>(0, static_cast<size_t>(z), c, t);
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            const size_t srcIndex = static_cast<size_t>(x + 1) + static_cast<size_t>(y + 1) * static_cast<size_t>(pw) +
                                    static_cast<size_t>(z + 1) * static_cast<size_t>(pw) * static_cast<size_t>(ph);
            dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
              padded[srcIndex] > 0 ? 1 : 0;
          }
        }
      }
    }
  }

  return out;
}

} // namespace nim
