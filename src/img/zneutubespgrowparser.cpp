#include "zneutubespgrowparser.h"

#include "zneutubeneighborhood.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

[[nodiscard]] double indexDistance(int64_t index1, int64_t index2, int width, int area)
{
  const int64_t indexDiff = std::llabs(index2 - index1);

  if (indexDiff == 1 || indexDiff == width || indexDiff == area) {
    return 1.0;
  }
  if (indexDiff == width + 1 || indexDiff == width - 1 || indexDiff == area + 1 || indexDiff == area - 1 ||
      indexDiff == area + width || indexDiff == area - width) {
    return 1.41421356237;
  }
  if (indexDiff == area + width + 1 || indexDiff == area + width - 1 || indexDiff == area - width - 1 ||
      indexDiff == area - width + 1) {
    return 1.73205080757;
  }

  throw ZException("Invalid input indices");
}

[[nodiscard]] double distanceWeight(double v)
{
  return std::sqrt(v);
}

} // namespace

ZNeutubeSpGrowParser::ZNeutubeSpGrowParser(SpGrowWorkspace& workspace)
  : m_workspace(workspace)
{}

ZNeutubeVoxelArray ZNeutubeSpGrowParser::extractPath(int64_t index) const
{
  ZNeutubeVoxelArray path;

  const int width = m_workspace.width;
  const int height = m_workspace.height;

  while (index >= 0) {
    ZNeutubeVoxel voxel;
    voxel.setFromIndex(static_cast<size_t>(index), width, height);
    path.append(voxel);

    if (!m_pathMask.isEmpty()) {
      const auto* mask = m_pathMask.timeData<uint8_t>(0);
      if (mask[static_cast<size_t>(index)] == 1) {
        break;
      }
    }

    const int64_t next = static_cast<int64_t>(m_workspace.path[static_cast<size_t>(index)]);
    CHECK(index != next);
    index = next;
  }

  return path;
}

double ZNeutubeSpGrowParser::pathLength(int64_t index, bool masked) const
{
  if (!m_workspace.length.empty()) {
    const int64_t originalIndex = index;
    if (masked) {
      if (!m_checkedMask.isEmpty()) {
        const uint8_t* maskArray = m_checkedMask.timeData<uint8_t>(0);
        const int* pathArray = m_workspace.path.data();
        while (index >= 0) {
          if (maskArray[static_cast<size_t>(index)] == 1) {
            break;
          }
          index = static_cast<int64_t>(pathArray[static_cast<size_t>(index)]);
        }
      } else {
        index = -1;
      }
    } else {
      index = -1;
    }

    double len = m_workspace.length[static_cast<size_t>(originalIndex)];
    if (index >= 0) {
      len -= m_workspace.length[static_cast<size_t>(index)];
    } else {
      len += 1.0;
    }
    return len;
  }

  // Fallback path length computation (rare for skeletonize; kept for parity).
  double len = 0.0;
  if (index >= 0) {
    if (m_workspace.mask[static_cast<size_t>(index)] == SP_GROW_SOURCE) {
      return 0.0;
    }

    len = 1.0;
    int64_t secondIndex = index;
    index = static_cast<int64_t>(m_workspace.path[static_cast<size_t>(index)]);

    while (index >= 0) {
      if (!m_checkedMask.isEmpty()) {
        if (m_checkedMask.timeData<uint8_t>(0)[static_cast<size_t>(index)] == 1) {
          break;
        }
      }

      const int64_t firstIndex = secondIndex;
      secondIndex = index;
      const int area = m_workspace.width * m_workspace.height;
      len += indexDistance(firstIndex, secondIndex, m_workspace.width, area);

      index = static_cast<int64_t>(m_workspace.path[static_cast<size_t>(index)]);
    }
  }

  return len;
}

ZNeutubeVoxelArray ZNeutubeSpGrowParser::extractLongestPath(double* length, bool masked)
{
  int64_t idx = -1;
  double maxLength = 0.0;

  if (m_fgArray.empty()) {
    const int conn = 6;
    const int width = m_workspace.width;
    const int height = m_workspace.height;
    const int depth = m_workspace.depth;
    const int area = width * height;

    const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
    std::vector<int> neighborOff;
    neighborOff.resize(nb.size());
    for (size_t j = 0; j < nb.size(); ++j) {
      const auto& o = nb.offset(j);
      neighborOff[j] = static_cast<int>(o.x) + static_cast<int>(o.y) * width + static_cast<int>(o.z) * area;
    }

    int fgCount = 0;
    for (size_t i = 0; i < m_workspace.size; ++i) {
      if (m_workspace.mask[i] == SP_GROW_BARRIER) {
        continue;
      }

      ++fgCount;

      const int z = static_cast<int>(i / static_cast<size_t>(area));
      const size_t rem = i - static_cast<size_t>(z) * static_cast<size_t>(area);
      const int y = static_cast<int>(rem / static_cast<size_t>(width));
      const int x = static_cast<int>(rem - static_cast<size_t>(y) * static_cast<size_t>(width));

      bool isBoundary = false;
      for (size_t n = 0; n < nb.size(); ++n) {
        const auto& o = nb.offset(n);
        const int nx = x + o.x;
        const int ny = y + o.y;
        const int nz = z + o.z;
        if (nx < 0 || ny < 0 || nz < 0 || nx >= width || ny >= height || nz >= depth) {
          isBoundary = true;
          break;
        }
        const size_t ni = static_cast<size_t>(static_cast<int64_t>(i) + neighborOff[n]);
        if (m_workspace.mask[ni] == SP_GROW_BARRIER) {
          isBoundary = true;
          break;
        }
      }

      if (isBoundary) {
        m_fgArray.push_back(static_cast<int64_t>(i));
      }
    }

    VLOG(1) << "Processing " << m_fgArray.size() << " points (" << fgCount << ")";
  }

  if (m_fgArray.empty()) {
    for (size_t i = 0; i < m_workspace.size; ++i) {
      const double len = pathLength(static_cast<int64_t>(i), masked);
      if (len > maxLength) {
        maxLength = len;
        idx = static_cast<int64_t>(i);
      }
    }
  } else {
    for (size_t i = 0; i < m_fgArray.size(); ++i) {
      const double len = pathLength(m_fgArray[i], masked);
      if (len > maxLength) {
        maxLength = len;
        idx = m_fgArray[i];
      }
    }
  }

  if (length != nullptr) {
    *length = maxLength;
  }

  return extractPath(idx);
}

std::vector<ZNeutubeVoxelArray> ZNeutubeSpGrowParser::extractAllPath(double minLength, const ZImg& ballImg)
{
  bool isPathAvailable = true;
  const double maskExpansionRadius = 2.0;
  const double skeletonRadius = 3.0;

  minLength -= maskExpansionRadius;

  std::vector<ZNeutubeVoxelArray> pathArray;

  while (isPathAvailable) {
    double length = 0.0;
    ZNeutubeVoxelArray path = extractLongestPath(&length, true);

    VLOG(1) << "Path: length = " << length << "; size = " << path.size() << "; Threshold = " << minLength;

    if (length < minLength || path.isEmpty()) {
      isPathAvailable = false;
    } else {
      pathArray.push_back(path);

      if (m_checkedMask.isEmpty()) {
        ZImgInfo info(static_cast<size_t>(m_workspace.width),
                      static_cast<size_t>(m_workspace.height),
                      static_cast<size_t>(m_workspace.depth),
                      1,
                      1,
                      1,
                      VoxelFormat::Unsigned);
        info.setVoxelFormat<uint8_t>();
        info.createDefaultDescriptions();
        m_checkedMask = ZImg(info);
        m_checkedMask.fill<uint8_t>(0);
      }

      if (m_pathMask.isEmpty()) {
        ZImgInfo info = m_checkedMask.info();
        m_pathMask = ZImg(info);
        m_pathMask.fill<uint8_t>(0);
      }

      path.sample(ballImg, distanceWeight);
      path.addValue(1.0);
      path.minimizeValue(skeletonRadius);
      path.labelImgWithBall(m_pathMask, 1);

      path.sample(ballImg, distanceWeight);
      path.addValue(maskExpansionRadius);
      path.labelImgWithBall(m_checkedMask, 1);
    }
  }

  return pathArray;
}

} // namespace nim
