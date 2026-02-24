#include "zneutubeneighborhood.h"

#include "zlog.h"

#include <array>
#include <vector>

namespace nim::neutube {

namespace {

// These offsets are intentionally ordered to match the legacy neuTube/neurolabi
// connectivity tables (tz_stack_neighborhood.c).
constexpr std::array<ZVoxelCoordinate, 8> kOffsets2d = {
  ZVoxelCoordinate(-1, 0, 0),
  ZVoxelCoordinate(1, 0, 0),
  ZVoxelCoordinate(0, -1, 0),
  ZVoxelCoordinate(0, 1, 0),
  ZVoxelCoordinate(-1, -1, 0),
  ZVoxelCoordinate(1, 1, 0),
  ZVoxelCoordinate(1, -1, 0),
  ZVoxelCoordinate(-1, 1, 0),
};

constexpr std::array<ZVoxelCoordinate, 26> kOffsets3d = {
  ZVoxelCoordinate(-1, 0, 0),   ZVoxelCoordinate(1, 0, 0),  ZVoxelCoordinate(0, -1, 0),  ZVoxelCoordinate(0, 1, 0),
  ZVoxelCoordinate(0, 0, -1),   ZVoxelCoordinate(0, 0, 1),

  ZVoxelCoordinate(-1, -1, 0),  ZVoxelCoordinate(1, 1, 0),  ZVoxelCoordinate(1, -1, 0),  ZVoxelCoordinate(-1, 1, 0),

  ZVoxelCoordinate(-1, 0, -1),  ZVoxelCoordinate(1, 0, 1),  ZVoxelCoordinate(1, 0, -1),  ZVoxelCoordinate(-1, 0, 1),

  ZVoxelCoordinate(0, -1, -1),  ZVoxelCoordinate(0, 1, 1),  ZVoxelCoordinate(0, 1, -1),  ZVoxelCoordinate(0, -1, 1),

  ZVoxelCoordinate(-1, -1, -1), ZVoxelCoordinate(1, 1, 1),  ZVoxelCoordinate(1, 1, -1),  ZVoxelCoordinate(-1, -1, 1),
  ZVoxelCoordinate(1, -1, -1),  ZVoxelCoordinate(-1, 1, 1), ZVoxelCoordinate(-1, 1, -1), ZVoxelCoordinate(1, -1, 1),
};

template<size_t N>
ZNeighborhood neighborhoodFromFirst(const std::array<ZVoxelCoordinate, N>& offsets, size_t count)
{
  CHECK(count <= offsets.size());
  std::vector<ZVoxelCoordinate> v;
  v.reserve(count);
  v.insert(v.end(), offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(count));
  return ZNeighborhood(v);
}

} // namespace

const ZNeighborhood& neighborhoodLegacyOrder(int connectivity)
{
  static const ZNeighborhood conn4 = neighborhoodFromFirst(kOffsets2d, 4);
  static const ZNeighborhood conn8 = neighborhoodFromFirst(kOffsets2d, 8);
  static const ZNeighborhood conn6 = neighborhoodFromFirst(kOffsets3d, 6);
  static const ZNeighborhood conn10 = neighborhoodFromFirst(kOffsets3d, 10);
  static const ZNeighborhood conn18 = neighborhoodFromFirst(kOffsets3d, 18);
  static const ZNeighborhood conn26 = neighborhoodFromFirst(kOffsets3d, 26);

  switch (connectivity) {
    case 4:
      return conn4;
    case 8:
      return conn8;
    case 6:
      return conn6;
    case 10:
      return conn10;
    case 18:
      return conn18;
    case 26:
      return conn26;
    default:
      CHECK(false) << "Unsupported connectivity: " << connectivity;
      return conn26;
  }
}

} // namespace nim::neutube
