#include "zneutubeimgbwmorph.h"

#include "zneutubeneighborhood.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>

namespace nim::neutube {

ZImg stackNotBinaryU8(const ZImg& in)
{
  if (in.isEmpty()) {
    return {};
  }
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<uint8_t>()) << "Expected uint8 input, got " << in.info();

  ZImg out = in;
  const size_t n = out.voxelNumber();
  auto* data = out.timeData<uint8_t>(0);
  for (size_t i = 0; i < n; ++i) {
    data[i] = (data[i] == 0) ? 1 : 0;
  }
  return out;
}

ZImg majorityFilterBinaryU8(const ZImg& in, int connectivity)
{
  if (in.isEmpty()) {
    return {};
  }
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<uint8_t>()) << "Expected uint8 input, got " << in.info();

  if (connectivity != 4 && connectivity != 8 && connectivity != 6 && connectivity != 18 && connectivity != 26 &&
      connectivity != 10) {
    throw ZException(fmt::format("Unsupported connectivity: {}", connectivity));
  }

  // Legacy Stack_Majority_Filter interprets 4/8 as 2D neighbors and applies it slice-by-slice.
  // We match that behavior by clamping to 4/8 when the input is 2D or when caller explicitly requests 2D.
  const bool is2dConn = (connectivity == 4 || connectivity == 8);
  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);

  ZImg out = in;

  const size_t width = out.width();
  const size_t height = out.height();
  const size_t depth = out.depth();

  const size_t conn = nb.size();
  const int halfConn = static_cast<int>(conn / 2);

  const auto* inData = in.timeData<uint8_t>(0);
  auto* outData = out.timeData<uint8_t>(0);

  auto inBound2d = [width, height](int x, int y) -> bool {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  };

  auto inBound3d = [width, height, depth](int x, int y, int z) -> bool {
    return x >= 0 && y >= 0 && z >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height) &&
           z < static_cast<int>(depth);
  };

  size_t offset = 0;
  for (size_t z = 0; z < depth; ++z) {
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x, ++offset) {
        if (inData[offset] == 0) {
          continue;
        }

        int n = 0;
        int nbound = 0;

        for (size_t b = 0; b < conn; ++b) {
          const ZVoxelCoordinate& o = nb.offset(b);
          const int nx = static_cast<int>(x) + o.x;
          const int ny = static_cast<int>(y) + o.y;
          const int nz = static_cast<int>(z) + o.z;

          bool ok = false;
          if (is2dConn) {
            ok = inBound2d(nx, ny);
            // For 2D connectivity, z offset must be zero by construction.
          } else {
            ok = inBound3d(nx, ny, nz);
          }

          if (!ok) {
            continue;
          }

          ++nbound;
          const size_t nidx =
            static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * width * height;
          if (inData[nidx] > 0) {
            ++n;
          }
        }

        if (nbound == static_cast<int>(conn)) {
          if (n <= halfConn) {
            outData[offset] = 0;
          }
        } else if (nbound > 0) {
          if (n <= nbound / 2) {
            outData[offset] = 0;
          }
        }
      }
    }
  }

  return out;
}

ZImg majorityFilterBinaryU8RLegacyLike(const ZImg& in, int connectivity, int mnbr)
{
  if (in.isEmpty()) {
    return {};
  }
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<uint8_t>()) << "Expected uint8 input, got " << in.info();

  if (connectivity != 4 && connectivity != 8 && connectivity != 6 && connectivity != 18 && connectivity != 26 &&
      connectivity != 10) {
    throw ZException(fmt::format("Unsupported connectivity: {}", connectivity));
  }

  if (mnbr <= 0) {
    // Legacy Stack_Majority_Filter_R returns a copy.
    return in;
  }

  if (mnbr > connectivity) {
    throw ZException(fmt::format("majorityFilterBinaryU8RLegacyLike: invalid mnbr={} for conn={}", mnbr, connectivity));
  }

  const bool is2dConn = (connectivity == 4 || connectivity == 8);
  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);

  ZImg out = in;

  const size_t width = out.width();
  const size_t height = out.height();
  const size_t depth = out.depth();

  const int conn = static_cast<int>(nb.size());
  CHECK(conn == connectivity) << "Unexpected neighborhood size: " << conn << " for connectivity " << connectivity;

  const auto* inData = in.timeData<uint8_t>(0);
  auto* outData = out.timeData<uint8_t>(0);

  auto inBound2d = [width, height](int x, int y) -> bool {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  };

  auto inBound3d = [width, height, depth](int x, int y, int z) -> bool {
    return x >= 0 && y >= 0 && z >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height) &&
           z < static_cast<int>(depth);
  };

  size_t offset = 0;
  for (size_t z = 0; z < depth; ++z) {
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x, ++offset) {
        if (inData[offset] == 0) {
          continue;
        }

        int n = 0;
        int nbound = 0;

        for (size_t b = 0; b < nb.size(); ++b) {
          const ZVoxelCoordinate& o = nb.offset(b);
          const int nx = static_cast<int>(x) + o.x;
          const int ny = static_cast<int>(y) + o.y;
          const int nz = static_cast<int>(z) + o.z;

          bool ok = false;
          if (is2dConn) {
            ok = inBound2d(nx, ny);
          } else {
            ok = inBound3d(nx, ny, nz);
          }

          if (!ok) {
            continue;
          }

          ++nbound;
          const size_t nidx =
            static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * width * height;
          if (inData[nidx] > 0) {
            ++n;
          }
        }

        if (nbound == conn) {
          if (n < mnbr) {
            outData[offset] = 0;
          }
        } else {
          if (n * conn < mnbr * nbound) {
            outData[offset] = 0;
          }
        }
      }
    }
  }

  return out;
}

ZImg fillHolesBinaryU8(const ZImg& in, int connectivity)
{
  if (in.isEmpty()) {
    return {};
  }
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);
  CHECK(in.isType<uint8_t>()) << "Expected uint8 input, got " << in.info();

  if (connectivity != 4 && connectivity != 8 && connectivity != 6 && connectivity != 10 && connectivity != 18 &&
      connectivity != 26) {
    throw ZException(fmt::format("Unsupported connectivity: {}", connectivity));
  }

  const size_t width = in.width();
  const size_t height = in.height();
  const size_t depth = in.depth();
  const size_t planeSize = width * height;
  const size_t voxelNumber = in.voxelNumber();

  // Work on a copy, like legacy Stack_Fill_Hole_N which starts from Copy_Stack(in).
  ZImg out = in;
  auto* outData = out.timeData<uint8_t>(0);

  // Seed queue with boundary background (0) voxels.
  std::vector<size_t> q;
  q.reserve(std::min(voxelNumber, 1024_uz));

  auto enqueueIfBackground = [&](size_t idx) {
    if (outData[idx] == 0) {
      outData[idx] = 1;
      q.push_back(idx);
    }
  };

  // Left/right planes (x = 0, x = width-1) for all y,z.
  if (width > 0) {
    for (size_t z = 0; z < depth; ++z) {
      const size_t zBase = z * planeSize;
      for (size_t y = 0; y < height; ++y) {
        const size_t rowBase = zBase + y * width;
        enqueueIfBackground(rowBase);
        if (width > 1) {
          enqueueIfBackground(rowBase + (width - 1));
        }
      }
    }
  }

  // Top/bottom planes (y = 0, y = height-1) for all x,z.
  if (height > 0) {
    for (size_t z = 0; z < depth; ++z) {
      const size_t zBase = z * planeSize;
      const size_t topBase = zBase;
      const size_t bottomBase = zBase + (height - 1) * width;
      for (size_t x = 1; x + 1 < width; ++x) {
        enqueueIfBackground(topBase + x);
        if (height > 1) {
          enqueueIfBackground(bottomBase + x);
        }
      }
    }
  }

  // For 3D connectivity, also seed front/back planes (z = 0, z = depth-1), excluding edges already covered above.
  if (connectivity == 6 || connectivity == 18 || connectivity == 26) {
    if (depth > 0) {
      const size_t frontBase = 0;
      const size_t backBase = (depth - 1) * planeSize;
      for (size_t y = 1; y + 1 < height; ++y) {
        for (size_t x = 1; x + 1 < width; ++x) {
          enqueueIfBackground(frontBase + y * width + x);
          if (depth > 1) {
            enqueueIfBackground(backBase + y * width + x);
          }
        }
      }
    }
  }

  const ZNeighborhood& nb = neighborhoodLegacyOrder(connectivity);
  const size_t conn = nb.size();

  auto inBound = [width, height, depth](int x, int y, int z) -> bool {
    return x >= 0 && y >= 0 && z >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height) &&
           z < static_cast<int>(depth);
  };

  // Flood fill boundary background.
  size_t head = 0;
  while (head < q.size()) {
    const size_t idx = q[head++];

    const size_t z = idx / planeSize;
    const size_t rem = idx - z * planeSize;
    const size_t y = rem / width;
    const size_t x = rem - y * width;

    for (size_t n = 0; n < conn; ++n) {
      const ZVoxelCoordinate& o = nb.offset(n);
      const int nx = static_cast<int>(x) + o.x;
      const int ny = static_cast<int>(y) + o.y;
      const int nz = static_cast<int>(z) + o.z;
      if (!inBound(nx, ny, nz)) {
        continue;
      }
      const size_t nidx =
        static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * planeSize;
      if (outData[nidx] == 0) {
        outData[nidx] = 1;
        q.push_back(nidx);
      }
    }
  }

  // Holes are background voxels not connected to the boundary.
  // Legacy does: Stack_Not(out); Stack_Or(in, out, out).
  ZImg holes = stackNotBinaryU8(out);

  ZImg filled = in;
  auto* filledData = filled.timeData<uint8_t>(0);
  const auto* holesData = holes.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    if (holesData[i] > 0) {
      filledData[i] = 1;
    }
  }

  return filled;
}

} // namespace nim::neutube
