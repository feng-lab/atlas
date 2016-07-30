#include "zneighborhood.h"
#include "zexception.h"

namespace nim {

ZNeighborhood::ZNeighborhood()
  : m_leftExtend(0), m_rightExtend(0), m_upExtend(0), m_downExtend(0), m_frontExtend(0), m_backExtend(0)
{
}

ZNeighborhood::ZNeighborhood(size_t nb)
{
  set(nb);
}

ZNeighborhood::ZNeighborhood(size_t xRadius, size_t yRadius, size_t zRadius, bool includeCenter)
{
  set(xRadius, yRadius, zRadius, includeCenter);
}

ZNeighborhood::ZNeighborhood(size_t left, size_t right, size_t up, size_t down, size_t front, size_t back,
                             bool includeCenter)
{
  set(left, right, up, down, front, back, includeCenter);
}

ZNeighborhood::ZNeighborhood(const std::vector<ZVoxelCoordinate>& offsets)
{
  set(offsets);
}

void ZNeighborhood::set(size_t nb)
{
  m_leftExtend = 1;
  m_rightExtend = 1;
  m_upExtend = 1;
  m_downExtend = 1;
  m_frontExtend = 1;
  m_backExtend = 1;
  m_offsets.resize(nb);
  switch (nb) {
    case 4:
      m_offsets[0] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[1] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[2] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[3] = ZVoxelCoordinate(0, 1, 0);
      m_frontExtend = 0;
      m_backExtend = 0;
      break;
    case 5:
      m_offsets[0] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[1] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[2] = ZVoxelCoordinate(0, 0, 0);
      m_offsets[3] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[4] = ZVoxelCoordinate(0, 1, 0);
      m_frontExtend = 0;
      m_backExtend = 0;
      break;
    case 8:
      set(1, 1, 0, false);
      return;
      break;
    case 9:
      set(1, 1, 0, true);
      return;
      break;
    case 6:
      m_offsets[0] = ZVoxelCoordinate(0, 0, -1);
      m_offsets[1] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[2] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[3] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[4] = ZVoxelCoordinate(0, 1, 0);
      m_offsets[5] = ZVoxelCoordinate(0, 0, 1);
      break;
    case 7:
      m_offsets[0] = ZVoxelCoordinate(0, 0, -1);
      m_offsets[1] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[2] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[3] = ZVoxelCoordinate(0, 0, 0);
      m_offsets[4] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[5] = ZVoxelCoordinate(0, 1, 0);
      m_offsets[6] = ZVoxelCoordinate(0, 0, 1);
      break;
    case 18:
      m_offsets[0] = ZVoxelCoordinate(0, -1, -1);
      m_offsets[1] = ZVoxelCoordinate(-1, 0, -1);
      m_offsets[2] = ZVoxelCoordinate(0, 0, -1);
      m_offsets[3] = ZVoxelCoordinate(1, 0, -1);
      m_offsets[4] = ZVoxelCoordinate(0, 1, -1);

      m_offsets[5] = ZVoxelCoordinate(-1, -1, 0);
      m_offsets[6] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[7] = ZVoxelCoordinate(1, -1, 0);
      m_offsets[8] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[9] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[10] = ZVoxelCoordinate(-1, 1, 0);
      m_offsets[11] = ZVoxelCoordinate(0, 1, 0);
      m_offsets[12] = ZVoxelCoordinate(1, 1, 0);

      m_offsets[13] = ZVoxelCoordinate(0, -1, 1);
      m_offsets[14] = ZVoxelCoordinate(-1, 0, 1);
      m_offsets[15] = ZVoxelCoordinate(0, 0, 1);
      m_offsets[16] = ZVoxelCoordinate(1, 0, 1);
      m_offsets[17] = ZVoxelCoordinate(0, 1, 1);
      break;
    case 19:
      m_offsets[0] = ZVoxelCoordinate(0, -1, -1);
      m_offsets[1] = ZVoxelCoordinate(-1, 0, -1);
      m_offsets[2] = ZVoxelCoordinate(0, 0, -1);
      m_offsets[3] = ZVoxelCoordinate(1, 0, -1);
      m_offsets[4] = ZVoxelCoordinate(0, 1, -1);

      m_offsets[5] = ZVoxelCoordinate(-1, -1, 0);
      m_offsets[6] = ZVoxelCoordinate(0, -1, 0);
      m_offsets[7] = ZVoxelCoordinate(1, -1, 0);
      m_offsets[8] = ZVoxelCoordinate(-1, 0, 0);
      m_offsets[9] = ZVoxelCoordinate(0, 0, 0);
      m_offsets[10] = ZVoxelCoordinate(1, 0, 0);
      m_offsets[11] = ZVoxelCoordinate(-1, 1, 0);
      m_offsets[12] = ZVoxelCoordinate(0, 1, 0);
      m_offsets[13] = ZVoxelCoordinate(1, 1, 0);

      m_offsets[14] = ZVoxelCoordinate(0, -1, 1);
      m_offsets[15] = ZVoxelCoordinate(-1, 0, 1);
      m_offsets[16] = ZVoxelCoordinate(0, 0, 1);
      m_offsets[17] = ZVoxelCoordinate(1, 0, 1);
      m_offsets[18] = ZVoxelCoordinate(0, 1, 1);
      break;
    case 26:
      set(1, 1, 1, false);
      return;
      break;
    case 27:
      set(1, 1, 1, true);
      return;
      break;
    default:
      throw ZImgException(QString("Not supported neighborhood option: %1").arg(nb));
      break;
  }
}

void ZNeighborhood::set(size_t xRadius, size_t yRadius, size_t zRadius, bool includeCenter)
{
  set(xRadius, xRadius, yRadius, yRadius, zRadius, zRadius, includeCenter);
}

void ZNeighborhood::set(size_t left, size_t right, size_t up, size_t down,
                        size_t front, size_t back, bool includeCenter)
{
  m_leftExtend = left;
  m_rightExtend = right;
  m_upExtend = up;
  m_downExtend = down;
  m_frontExtend = front;
  m_backExtend = back;
  m_offsets.clear();
  for (int zoffset = -static_cast<int>(front); zoffset < 1 + static_cast<int>(back); ++zoffset) {
    for (int yoffset = -static_cast<int>(up); yoffset < 1 + static_cast<int>(down); ++yoffset) {
      for (int xoffset = -static_cast<int>(left); xoffset < 1 + static_cast<int>(right); ++xoffset) {
        if (xoffset == 0 && yoffset == 0 && zoffset == 0 && !includeCenter)
          continue;
        m_offsets.emplace_back(xoffset, yoffset, zoffset);
      }
    }
  }
}

void ZNeighborhood::set(const std::vector<ZVoxelCoordinate>& offsets)
{
  m_offsets = offsets;
  if (m_offsets.empty()) {
    m_leftExtend = 0;
    m_rightExtend = 0;
    m_upExtend = 0;
    m_downExtend = 0;
    m_frontExtend = 0;
    m_backExtend = 0;
  } else {
    ZVoxelCoordinate minOffset = m_offsets[0];
    ZVoxelCoordinate maxOffset = minOffset;
    for (size_t i = 1; i < m_offsets.size(); ++i) {
      minOffset = min(minOffset, m_offsets[i]);
      maxOffset = max(maxOffset, m_offsets[i]);
    }
    m_leftExtend = std::max(0, -(minOffset.x));
    m_rightExtend = std::max(0, maxOffset.x);
    m_upExtend = std::max(0, -(minOffset.y));
    m_downExtend = std::max(0, maxOffset.y);
    m_frontExtend = std::max(0, -(minOffset.z));
    m_backExtend = std::max(0, maxOffset.z);
  }
}

void ZNeighborhood::removeSymmetricalOffsets()
{
  size_t numRemoved = 0;
  // 0 to (size() - 1 - numRemoved) are valid offsets
  for (size_t i = m_offsets.size() - 1; i > 0; --i) {
    for (size_t j = 0; j < i; ++j) {
      if (m_offsets[j] + m_offsets[i] == ZVoxelCoordinate()) {
        std::swap(m_offsets[i], m_offsets[m_offsets.size() - 1 - numRemoved]);
        ++numRemoved;
        break;
      }
    }
  }
  m_offsets.resize(m_offsets.size() - numRemoved);
}

void ZNeighborhood::removeCenter()
{
  m_offsets.erase(std::remove(m_offsets.begin(), m_offsets.end(), ZVoxelCoordinate()), m_offsets.end());
}

} // namespace nim
