#include "z3dpickingmanager.h"

#include "z3dgl.h"
#include "z3dtexture.h"
#include "zlog.h"
#include "zvulkantexture.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] uint64_t unsignedDistance(int64_t lhs, int64_t rhs) noexcept
{
  const uint64_t lhsBits = static_cast<uint64_t>(lhs);
  const uint64_t rhsBits = static_cast<uint64_t>(rhs);
  return lhs >= rhs ? lhsBits - rhsBits : rhsBits - lhsBits;
}

void checkSquaredDistanceRange(uint64_t maximumDx, uint64_t maximumDy)
{
  constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
  CHECK(maximumDx == 0u || maximumDx <= maximum / maximumDx)
    << "Vulkan picking x distance exceeds the squared-distance representation";
  CHECK(maximumDy == 0u || maximumDy <= maximum / maximumDy)
    << "Vulkan picking y distance exceeds the squared-distance representation";
  const uint64_t maximumDxSquared = maximumDx * maximumDx;
  const uint64_t maximumDySquared = maximumDy * maximumDy;
  CHECK_LE(maximumDySquared, maximum - maximumDxSquared)
    << "Vulkan picking distance exceeds the squared-distance representation";
}

} // namespace

glm::uvec2 Z3DPickingManager::ImageDepthSample::samplePixelForPhysicalInput(glm::ivec2 physicalInputPosition,
                                                                            glm::uvec2 fullPhysicalExtent)
{
  CHECK_GT(fullPhysicalExtent.x, 0u);
  CHECK_GT(fullPhysicalExtent.y, 0u);
  CHECK_LE(fullPhysicalExtent.x, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_LE(fullPhysicalExtent.y, static_cast<uint32_t>(std::numeric_limits<int>::max()));

  const int64_t maximumX = static_cast<int64_t>(fullPhysicalExtent.x) - 1;
  const int64_t maximumY = static_cast<int64_t>(fullPhysicalExtent.y) - 1;
  const int64_t sampleX = std::clamp<int64_t>(physicalInputPosition.x, 0, maximumX);
  const int64_t bottomLeftY =
    std::clamp<int64_t>(static_cast<int64_t>(fullPhysicalExtent.y) - static_cast<int64_t>(physicalInputPosition.y),
                        0,
                        maximumY);
  const int64_t sampleY = maximumY - bottomLeftY;
  return glm::uvec2(static_cast<uint32_t>(sampleX), static_cast<uint32_t>(sampleY));
}

glm::ivec2 Z3DPickingManager::ImageDepthSample::bottomLeftUnprojectionPixel() const
{
  CHECK_GT(fullPhysicalExtent.x, 0u);
  CHECK_GT(fullPhysicalExtent.y, 0u);
  CHECK_LE(fullPhysicalExtent.x, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_LE(fullPhysicalExtent.y, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_LT(topLeftPhysicalPixel.x, fullPhysicalExtent.x);
  CHECK_LT(topLeftPhysicalPixel.y, fullPhysicalExtent.y);
  return glm::ivec2(static_cast<int>(topLeftPhysicalPixel.x),
                    static_cast<int>(fullPhysicalExtent.y - 1u - topLeftPhysicalPixel.y));
}

Z3DPickingManager::ScopedQueryOverride::ScopedQueryOverride(Z3DPickingManager& manager,
                                                            const QueryOverride& queryOverride)
  : m_manager(&manager)
  , m_queryOverride(&queryOverride)
{
  m_manager->beginQueryOverride(queryOverride);
}

Z3DPickingManager::ScopedQueryOverride::~ScopedQueryOverride()
{
  CHECK(m_manager != nullptr);
  CHECK(m_queryOverride != nullptr);
  m_manager->endQueryOverride(*m_queryOverride);
}

void Z3DPickingManager::beginQueryOverride(const QueryOverride& queryOverride)
{
  CHECK(m_queryOverride == nullptr) << "Picking query overrides cannot be nested";
  CHECK(static_cast<bool>(queryOverride.objectAtWidgetPos)) << "Picking query override requires objectAtWidgetPos";
  CHECK(static_cast<bool>(queryOverride.depthAtWidgetPos)) << "Picking query override requires depthAtWidgetPos";
  CHECK(static_cast<bool>(queryOverride.sortObjectsByDistanceToPos))
    << "Picking query override requires sortObjectsByDistanceToPos";
  CHECK(static_cast<bool>(queryOverride.imageDepthAtPhysicalInput))
    << "Picking query override requires imageDepthAtPhysicalInput";
  m_queryOverride = &queryOverride;
}

void Z3DPickingManager::endQueryOverride(const QueryOverride& queryOverride)
{
  CHECK(m_queryOverride != nullptr) << "Picking query override scope is not active";
  CHECK(m_queryOverride == &queryOverride) << "Picking query override scope ended out of order";
  m_queryOverride = nullptr;
}

void Z3DPickingManager::checkVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const
{
  CHECK(hasVulkanTarget()) << "Vulkan attachment query requires a picking target";
  CHECK_LE(m_vkSize.x, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_LE(m_vkSize.y, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_GE(topLeftPixel.x, 0) << "Vulkan attachment pixel x must be non-negative";
  CHECK_GE(topLeftPixel.y, 0) << "Vulkan attachment pixel y must be non-negative";
  CHECK_LT(static_cast<uint32_t>(topLeftPixel.x), m_vkSize.x)
    << "Vulkan attachment pixel x is outside the picking target";
  CHECK_LT(static_cast<uint32_t>(topLeftPixel.y), m_vkSize.y)
    << "Vulkan attachment pixel y is outside the picking target";
}

const uint8_t* Z3DPickingManager::currentCachedVulkanColorData() const noexcept
{
  if (!m_cachedColorValid || m_cachedColorSize != m_vkSize) {
    return nullptr;
  }
  const size_t requiredBytes = static_cast<size_t>(m_vkSize.x) * static_cast<size_t>(m_vkSize.y) * 4u;
  return m_cachedColor.size() == requiredBytes ? m_cachedColor.data() : nullptr;
}

void Z3DPickingManager::setPickingTarget(Z3DRenderTarget& rt)
{
  CHECK(rt.attachment(GL_COLOR_ATTACHMENT0)->internalFormat() == GL_RGBA8);
  m_renderTarget = &rt;
  // Switching to a GL picking target should invalidate any Vulkan pointers
  // and cached mappings so subsequent queries do not read stale data.
  clearVulkanState();
}

void Z3DPickingManager::setPickingTarget(ZVulkanTexture& color,
                                         ZVulkanTexture& depth,
                                         const glm::uvec2& size)
{
  m_renderTarget = nullptr;

  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);

  const bool attachmentsChanged =
    (m_vkColor != &color) || (m_vkDepth != &depth) || (m_vkSize != size);

  m_vkColor = &color;
  m_vkDepth = &depth;
  m_vkSize = size;

  if (attachmentsChanged) {
    m_cachedColorValid = false;
    m_cachedColor.clear();
    m_cachedColorSize = glm::uvec2(0u);
  }
}

glm::col4 Z3DPickingManager::registerObject(const void* obj, size_t objectId)
{
  CHECK(obj != nullptr);
  const PickingObject object{objectId, obj};
  increaseColor();
  const bool inserted = m_colorToObject.emplace(m_currentColor, object).second;
  CHECK(inserted) << "Picking colors exhausted their unique range";
  return m_currentColor;
}

void Z3DPickingManager::deregisterObject(const glm::col4& col)
{
  const auto colorIt = m_colorToObject.find(col);
  CHECK(colorIt != m_colorToObject.end()) << "Cannot deregister an unknown picking color";
  m_colorToObject.erase(colorIt);
}

Z3DPickingManager::PickingObject Z3DPickingManager::pickingObjectOfColor(const glm::col4& col) const
{
  const std::optional<PickingObject> object = registeredObjectForColor(col);
  return object.value_or(PickingObject{});
}

std::optional<Z3DPickingManager::PickingObject>
Z3DPickingManager::registeredObjectForColor(const glm::col4& color) const
{
  if (color.a == 0u) {
    return std::nullopt;
  }

  const auto it = m_colorToObject.find(color);
  return it == m_colorToObject.end() ? std::nullopt : std::optional<PickingObject>(it->second);
}

Z3DPickingManager::PickingObject Z3DPickingManager::objectAtVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const
{
  checkVulkanAttachmentPixel(topLeftPixel);

  const int width = static_cast<int>(m_vkSize.x);
  const int bottomLeftY = static_cast<int>(m_vkSize.y) - 1 - topLeftPixel.y;
  uint8_t rgba[4] = {0u, 0u, 0u, 0u};
  if (const uint8_t* cachedColor = currentCachedVulkanColorData(); cachedColor != nullptr) {
    const size_t pixelIndex =
      static_cast<size_t>(bottomLeftY) * static_cast<size_t>(width) + static_cast<size_t>(topLeftPixel.x);
    std::memcpy(rgba, cachedColor + 4u * pixelIndex, sizeof(rgba));
  } else {
    m_vkColor->downloadSubImage(rgba,
                                sizeof(rgba),
                                vk::Offset3D{topLeftPixel.x, bottomLeftY, 0},
                                vk::Extent3D{1u, 1u, 1u},
                                vk::ImageAspectFlagBits::eColor);
  }
  const std::optional<PickingObject> object = registeredObjectForColor(glm::col4{rgba[0], rgba[1], rgba[2], rgba[3]});
  return object.value_or(PickingObject{});
}

GLfloat Z3DPickingManager::depthAtVulkanAttachmentPixel(glm::ivec2 topLeftPixel) const
{
  checkVulkanAttachmentPixel(topLeftPixel);

  const int bottomLeftY = static_cast<int>(m_vkSize.y) - 1 - topLeftPixel.y;
  const vk::Offset3D offset{topLeftPixel.x, bottomLeftY, 0};
  if (m_vkDepth->format() == vk::Format::eD32Sfloat) {
    float depth = 1.0f;
    m_vkDepth->downloadSubImage(&depth,
                                sizeof(depth),
                                offset,
                                vk::Extent3D{1u, 1u, 1u},
                                vk::ImageAspectFlagBits::eDepth);
    return depth;
  }

  // The picking target's other supported depth representation is D24UnormS8.
  uint32_t packed = 0u;
  m_vkDepth->downloadSubImage(&packed,
                              sizeof(packed),
                              offset,
                              vk::Extent3D{1u, 1u, 1u},
                              vk::ImageAspectFlagBits::eDepth);
  return static_cast<float>(packed & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
}

std::vector<Z3DPickingManager::VulkanObjectHit>
Z3DPickingManager::objectsNearVulkanAttachmentReference(glm::i64vec2 topLeftReferencePixel,
                                                        glm::uvec4 topLeftSearchRect) const
{
  CHECK(hasVulkanTarget()) << "Vulkan attachment query requires a picking target";
  CHECK_LE(m_vkSize.x, static_cast<uint32_t>(std::numeric_limits<int>::max()));
  CHECK_LE(m_vkSize.y, static_cast<uint32_t>(std::numeric_limits<int>::max()));

  const int width = static_cast<int>(m_vkSize.x);
  const int height = static_cast<int>(m_vkSize.y);
  CHECK_GT(topLeftSearchRect.z, 0u);
  CHECK_GT(topLeftSearchRect.w, 0u);
  const uint64_t searchEndX = static_cast<uint64_t>(topLeftSearchRect.x) + topLeftSearchRect.z;
  const uint64_t searchEndY = static_cast<uint64_t>(topLeftSearchRect.y) + topLeftSearchRect.w;
  CHECK_LE(searchEndX, m_vkSize.x);
  CHECK_LE(searchEndY, m_vkSize.y);
  const int xBegin = static_cast<int>(topLeftSearchRect.x);
  const int xEnd = static_cast<int>(searchEndX - 1u);
  const int topYBegin = static_cast<int>(topLeftSearchRect.y);
  const int topYEnd = static_cast<int>(searchEndY - 1u);
  const int yBegin = height - 1 - topYEnd;
  const int yEnd = height - 1 - topYBegin;

  const uint64_t maximumDx =
    std::max(unsignedDistance(topLeftReferencePixel.x, xBegin), unsignedDistance(topLeftReferencePixel.x, xEnd));
  const uint64_t maximumDy =
    std::max(unsignedDistance(topLeftReferencePixel.y, topYBegin), unsignedDistance(topLeftReferencePixel.y, topYEnd));
  checkSquaredDistanceRange(maximumDx, maximumDy);

  std::vector<uint8_t> downloadedColor;
  const uint8_t* colorData = currentCachedVulkanColorData();
  if (colorData == nullptr) {
    const size_t byteCount = static_cast<size_t>(m_vkSize.x) * static_cast<size_t>(m_vkSize.y) * 4u;
    downloadedColor.resize(byteCount);
    m_vkColor->downloadData(downloadedColor.data(), downloadedColor.size());
    colorData = downloadedColor.data();
  }

  boost::unordered_flat_map<glm::col4, uint64_t> nearestDistanceByColor;
  for (int y = yBegin; y <= yEnd; ++y) {
    for (int x = xBegin; x <= xEnd; ++x) {
      const size_t pixelIndex = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
      const uint8_t* rgba = colorData + 4u * pixelIndex;
      const glm::col4 color{rgba[0], rgba[1], rgba[2], rgba[3]};
      if (color.a == 0u) {
        continue;
      }

      const int topY = height - 1 - y;
      const uint64_t dx = unsignedDistance(topLeftReferencePixel.x, x);
      const uint64_t dy = unsignedDistance(topLeftReferencePixel.y, topY);
      const uint64_t squaredDistance = dx * dx + dy * dy;
      auto [it, inserted] = nearestDistanceByColor.try_emplace(color, squaredDistance);
      if (!inserted && squaredDistance < it->second) {
        it->second = squaredDistance;
      }
    }
  }

  std::vector<VulkanObjectHit> hits;
  hits.reserve(nearestDistanceByColor.size());
  for (const auto& [color, squaredDistance] : nearestDistanceByColor) {
    if (const std::optional<PickingObject> object = registeredObjectForColor(color); object.has_value()) {
      hits.push_back(VulkanObjectHit{object->objectId, object->object, squaredDistance});
    }
  }
  std::ranges::sort(hits, {}, &VulkanObjectHit::squaredPhysicalPixelDistance);
  return hits;
}

Z3DPickingManager::PickingObject Z3DPickingManager::pickingObjectAtWidgetPos(glm::ivec2 pos)
{
  if (m_queryOverride != nullptr) {
    return m_queryOverride->objectAtWidgetPos(pos);
  }

  assert(m_devicePixelRatio >= 1);
  pos[0] = pos[0] * m_devicePixelRatio;
  pos[1] = pos[1] * m_devicePixelRatio;

  // Vulkan path
  if (m_vkColor) {
    const int w = static_cast<int>(m_vkSize.x);
    const int h = static_cast<int>(m_vkSize.y);
    if (w <= 0 || h <= 0) {
      return {};
    }
    // Clamp inside bounds
    pos.x = std::clamp(pos.x, 0, w - 1);
    pos.y = std::clamp(pos.y, 0, h - 1);
    try {
      return objectAtVulkanAttachmentPixel(pos);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking color download failed: " << e.what();
      return {};
    }
  }

  if (!m_renderTarget) {
    return {};
  }
  auto texSize = glm::ivec3(m_renderTarget->attachment(GL_COLOR_ATTACHMENT0)->dimension());
  pos[1] = texSize[1] - pos[1];
  return pickingObjectOfColor(m_renderTarget->colorAtPos(pos));
}

const void* Z3DPickingManager::objectAtWidgetPos(glm::ivec2 pos)
{
  return pickingObjectAtWidgetPos(pos).object;
}

GLfloat Z3DPickingManager::depthAtWidgetPos(glm::ivec2 pos)
{
  if (m_queryOverride != nullptr) {
    return m_queryOverride->depthAtWidgetPos(pos);
  }

  assert(m_devicePixelRatio >= 1);
  pos[0] = pos[0] * m_devicePixelRatio;
  pos[1] = pos[1] * m_devicePixelRatio;

  // Vulkan path
  if (m_vkDepth) {
    const int w = static_cast<int>(m_vkSize.x);
    const int h = static_cast<int>(m_vkSize.y);
    if (w <= 0 || h <= 0) {
      return 1.0f;
    }
    // Clamp inside bounds
    pos.x = std::clamp(pos.x, 0, w - 1);
    pos.y = std::clamp(pos.y, 0, h - 1);
    try {
      return depthAtVulkanAttachmentPixel(pos);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking depth download failed: " << e.what();
      return 1.0f;
    }
  }

  if (!m_renderTarget) {
    return 1.0f;
  }
  auto texSize = glm::ivec2(m_renderTarget->size());
  pos[1] = texSize[1] - pos[1];
  return m_renderTarget->depthAtPos(pos);
}

std::vector<Z3DPickingManager::PickingObject>
Z3DPickingManager::sortPickingObjectsByDistanceToPos(const glm::ivec2& pos, int radius, bool ascend)
{
  if (m_queryOverride != nullptr) {
    return m_queryOverride->sortObjectsByDistanceToPos(pos, radius, ascend);
  }

  if (!m_renderTarget && !m_vkColor) {
    return {};
  }

  // Convert widget-space logical pixels to physical picking pixels.
  glm::ivec2 physPos = pos;
  physPos.x = static_cast<int>(physPos.x * m_devicePixelRatio);
  physPos.y = static_cast<int>(physPos.y * m_devicePixelRatio);

  int physRadius = radius;
  if (physRadius >= 0) {
    physRadius = static_cast<int>(std::ceil(static_cast<double>(physRadius) * m_devicePixelRatio));
  }

  if (m_vkColor) {
    const int width = static_cast<int>(m_vkSize.x);
    const int height = static_cast<int>(m_vkSize.y);
    if (width <= 0 || height <= 0) {
      return {};
    }
    physPos.x = std::clamp(physPos.x, 0, width - 1);
    physPos.y = std::clamp(physPos.y, 0, height - 1);

    std::vector<VulkanObjectHit> hits;
    try {
      checkVulkanAttachmentPixel(physPos);
      const std::optional<uint32_t> strictRadius =
        physRadius < 0 ? std::nullopt : std::optional<uint32_t>(static_cast<uint32_t>(physRadius));
      glm::uvec4 searchRect{0u, 0u, m_vkSize.x, m_vkSize.y};
      if (strictRadius.has_value()) {
        const uint32_t radiusValue = *strictRadius;
        const uint32_t beginX =
          static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(physPos.x) - radiusValue));
        const uint32_t beginY =
          static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(physPos.y) - radiusValue));
        const uint32_t endX =
          static_cast<uint32_t>(std::min<uint64_t>(m_vkSize.x - 1u, static_cast<uint64_t>(physPos.x) + radiusValue));
        const uint32_t endY =
          static_cast<uint32_t>(std::min<uint64_t>(m_vkSize.y - 1u, static_cast<uint64_t>(physPos.y) + radiusValue));
        searchRect = glm::uvec4(beginX, beginY, endX - beginX + 1u, endY - beginY + 1u);
      }
      hits = objectsNearVulkanAttachmentReference(glm::i64vec2(physPos), searchRect);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking color download failed: " << e.what();
      return {};
    }

    std::vector<PickingObject> result;
    result.reserve(hits.size());
    if (ascend) {
      for (const VulkanObjectHit& hit : hits) {
        result.push_back(PickingObject{hit.objectId, hit.object});
      }
    } else {
      for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        result.push_back(PickingObject{it->objectId, it->object});
      }
    }
    return result;
  }

  boost::unordered_flat_map<glm::col4, int> col2dist;

  auto recordColor = [&](int dx, int dy, const glm::col4& col) {
    if (col.a == 0) {
      return;
    }
    const int dist = dx * dx + dy * dy;
    auto it = col2dist.find(col);
    if (it == col2dist.end()) {
      col2dist.emplace(col, dist);
    } else {
      it->second = std::min(it->second, dist);
    }
  };

  CHECK(m_renderTarget);
  const Z3DTexture* tex = m_renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  const int width = static_cast<int>(m_renderTarget->size().x);
  const int height = static_cast<int>(m_renderTarget->size().y);
  if (width <= 0 || height <= 0) {
    return {};
  }

  physPos.x = std::clamp(physPos.x, 0, width - 1);
  physPos.y = std::clamp(physPos.y, 0, height - 1);

  // GL path: match objectAtWidgetPos's y-flip convention.
  const int baseY = height - physPos.y;

  if (physRadius < 0) {
    physRadius = std::max(width, height);
  }

  const GLenum dataFormat = GL_BGRA;
  const GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;
  auto glBuf =
    std::make_unique_for_overwrite<glm::col4[]>(Z3DTexture::bypePerPixel(dataFormat, dataType) * tex->numPixels() / 4);
  tex->downloadTextureToBuffer(dataFormat, dataType, glBuf.get());

  for (int y = std::max(0, baseY - physRadius); y <= std::min(height - 1, baseY + physRadius); ++y) {
    for (int x = std::max(0, physPos.x - physRadius); x <= std::min(width - 1, physPos.x + physRadius); ++x) {
      glm::col4 color = glBuf[(y * width) + x];
      std::swap(color.r, color.b);
      recordColor(x - physPos.x, y - baseY, color);
    }
  }

  std::vector<PickingObject> res;
  if (ascend) {
    std::multimap<int, PickingObject> dist2obj;
    for (auto& [color, dist] : col2dist) {
      const PickingObject object = pickingObjectOfColor(color);
      if (object.object != nullptr) {
        dist2obj.emplace(dist, object);
      }
    }
    for (auto& it : dist2obj) {
      res.push_back(it.second);
    }
  } else {
    std::multimap<int, PickingObject, std::greater<>> dist2obj;
    for (auto& [color, dist] : col2dist) {
      const PickingObject object = pickingObjectOfColor(color);
      if (object.object != nullptr) {
        dist2obj.emplace(dist, object);
      }
    }
    for (auto& it : dist2obj) {
      res.push_back(it.second);
    }
  }
  return res;
}

std::optional<Z3DPickingManager::ImageDepthSample>
Z3DPickingManager::imageDepthAtPhysicalInput(size_t imageObjectId, glm::ivec2 physicalInputPosition)
{
  if (m_queryOverride == nullptr) {
    return std::nullopt;
  }
  return m_queryOverride->imageDepthAtPhysicalInput(imageObjectId, physicalInputPosition);
}

void Z3DPickingManager::resetRenderTarget()
{
  m_renderTarget = nullptr;
  clearVulkanState();
}

void Z3DPickingManager::clearVulkanState()
{
  m_vkColor = nullptr;
  m_vkDepth = nullptr;
  m_vkSize = glm::uvec2(0u);
  m_cachedColorValid = false;
  m_cachedColor.clear();
  m_cachedColorSize = glm::uvec2(0u);
}

void Z3DPickingManager::clearTarget()
{
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Z3DPickingManager::increaseColor()
{
  auto col = std::bit_cast<uint32_t>(m_currentColor);
  CHECK_NE(col, std::numeric_limits<uint32_t>::max()) << "Picking colors exhausted their unique range";
  ++col;
  m_currentColor = std::bit_cast<glm::col4>(col);
}

} // namespace nim
