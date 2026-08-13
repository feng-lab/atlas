#include "z3dtiledescriptor.h"

#include <algorithm>
#include <limits>

namespace nim {

namespace {

uint64_t divideRoundUp(uint32_t value, uint32_t divisor)
{
  CHECK_GT(value, 0u);
  CHECK_GT(divisor, 0u);
  return 1u + (static_cast<uint64_t>(value) - 1u) / divisor;
}

void checkAttachmentExtent(uint32_t validExtent, uint32_t guardPixels)
{
  const uint64_t expandedExtent = static_cast<uint64_t>(validExtent) + 2u * static_cast<uint64_t>(guardPixels);
  CHECK_LE(expandedExtent, std::numeric_limits<uint32_t>::max())
    << "Tile attachment extent exceeds the representable extent";
}

} // namespace

Z3DTileDescriptor::Z3DTileDescriptor(glm::uvec2 fullOutputExtent,
                                     glm::uvec2 validOutputOrigin,
                                     glm::uvec2 validOutputExtent,
                                     uint32_t guardPixels)
  : m_fullOutputExtent(fullOutputExtent)
  , m_validOutputOrigin(validOutputOrigin)
  , m_validOutputExtent(validOutputExtent)
  , m_guardPixels(guardPixels)
{
  CHECK_GT(m_fullOutputExtent.x, 0u) << "Tiled output width must be positive";
  CHECK_GT(m_fullOutputExtent.y, 0u) << "Tiled output height must be positive";
  CHECK_GT(m_validOutputExtent.x, 0u) << "Tile valid width must be positive";
  CHECK_GT(m_validOutputExtent.y, 0u) << "Tile valid height must be positive";
  CHECK_LT(m_validOutputOrigin.x, m_fullOutputExtent.x);
  CHECK_LT(m_validOutputOrigin.y, m_fullOutputExtent.y);
  CHECK_LE(m_validOutputExtent.x, m_fullOutputExtent.x - m_validOutputOrigin.x);
  CHECK_LE(m_validOutputExtent.y, m_fullOutputExtent.y - m_validOutputOrigin.y);
  checkAttachmentExtent(m_validOutputExtent.x, m_guardPixels);
  checkAttachmentExtent(m_validOutputExtent.y, m_guardPixels);
}

std::vector<Z3DTileDescriptor>
makeZ3DTileDescriptors(glm::uvec2 fullOutputExtent, glm::uvec2 tileExtent, uint32_t guardPixels)
{
  CHECK_GT(fullOutputExtent.x, 0u) << "Tiled output width must be positive";
  CHECK_GT(fullOutputExtent.y, 0u) << "Tiled output height must be positive";
  CHECK_GT(tileExtent.x, 0u) << "Tile width must be positive";
  CHECK_GT(tileExtent.y, 0u) << "Tile height must be positive";

  const uint64_t columnCount = divideRoundUp(fullOutputExtent.x, tileExtent.x);
  const uint64_t rowCount = divideRoundUp(fullOutputExtent.y, tileExtent.y);
  CHECK_LE(columnCount, std::numeric_limits<uint64_t>::max() / rowCount) << "Tile count overflow";
  const uint64_t tileCount = columnCount * rowCount;

  std::vector<Z3DTileDescriptor> descriptors;
  CHECK_LE(tileCount, descriptors.max_size()) << "Tile count exceeds the platform container capacity";
  descriptors.reserve(static_cast<size_t>(tileCount));

  for (uint64_t row = 0; row < rowCount; ++row) {
    const bool forward = (row % 2u) == 0u;
    for (uint64_t columnOffset = 0; columnOffset < columnCount; ++columnOffset) {
      const uint64_t column = forward ? columnOffset : columnCount - 1u - columnOffset;
      const uint64_t originX64 = column * static_cast<uint64_t>(tileExtent.x);
      const uint64_t originY64 = row * static_cast<uint64_t>(tileExtent.y);
      CHECK_LT(originX64, fullOutputExtent.x);
      CHECK_LT(originY64, fullOutputExtent.y);

      const auto originX = static_cast<uint32_t>(originX64);
      const auto originY = static_cast<uint32_t>(originY64);
      const uint32_t validWidth = std::min(tileExtent.x, fullOutputExtent.x - originX);
      const uint32_t validHeight = std::min(tileExtent.y, fullOutputExtent.y - originY);
      descriptors.emplace_back(fullOutputExtent,
                               glm::uvec2(originX, originY),
                               glm::uvec2(validWidth, validHeight),
                               guardPixels);
    }
  }

  CHECK_EQ(descriptors.size(), tileCount);
  return descriptors;
}

std::vector<Z3DTileDescriptor>
makeZ3DFixedRegionDescriptors(glm::uvec2 fullOutputExtent, size_t regionCount, uint32_t guardPixels)
{
  CHECK_GT(fullOutputExtent.x, 0u) << "Fixed-region output width must be positive";
  CHECK_GT(fullOutputExtent.y, 0u) << "Fixed-region output height must be positive";
  CHECK_GT(regionCount, 0u) << "Fixed-region count must be positive";
  CHECK_LE(regionCount, static_cast<size_t>(fullOutputExtent.x))
    << "Fixed-region count cannot exceed the physical output width";

  std::vector<Z3DTileDescriptor> descriptors;
  CHECK_LE(regionCount, descriptors.max_size()) << "Fixed-region count exceeds the platform container capacity";
  descriptors.reserve(regionCount);

  const uint32_t count = static_cast<uint32_t>(regionCount);
  const uint32_t baseWidth = fullOutputExtent.x / count;
  const uint32_t widerRegionCount = fullOutputExtent.x % count;
  uint32_t originX = 0u;
  for (uint32_t regionIndex = 0u; regionIndex < count; ++regionIndex) {
    const uint32_t regionWidth = baseWidth + (regionIndex < widerRegionCount ? 1u : 0u);
    descriptors.emplace_back(fullOutputExtent,
                             glm::uvec2(originX, 0u),
                             glm::uvec2(regionWidth, fullOutputExtent.y),
                             guardPixels);
    originX += regionWidth;
  }

  CHECK_EQ(descriptors.size(), regionCount);
  CHECK_EQ(originX, fullOutputExtent.x);
  return descriptors;
}

} // namespace nim
