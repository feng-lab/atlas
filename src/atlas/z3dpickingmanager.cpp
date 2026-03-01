#include "z3dpickingmanager.h"

#include "z3dgl.h"
#include "z3dtexture.h"
#include "zlog.h"
#include "zvulkantexture.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace nim {

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

glm::col4 Z3DPickingManager::registerObject(const void* obj)
{
  increaseColor();
  m_colorToObject[m_currentColor] = obj;
  m_objectToColor[obj] = m_currentColor;
  return m_currentColor;
}

void Z3DPickingManager::deregisterObject(const void* obj)
{
  glm::col4 col = colorOfObject(obj);
  m_colorToObject.erase(col);
  m_objectToColor.erase(obj);
}

void Z3DPickingManager::deregisterObject(const glm::col4& col)
{
  const void* obj = objectOfColor(col);
  m_colorToObject.erase(col);
  m_objectToColor.erase(obj);
}

void Z3DPickingManager::clearRegisteredObjects()
{
  m_colorToObject.clear();
  m_objectToColor.clear();
  m_currentColor = glm::col4(0, 0, 0, 128);
}

glm::col4 Z3DPickingManager::colorOfObject(const void* obj)
{
  if (!obj) {
    return glm::col4(0, 0, 0, 0);
  }

  if (isRegistered(obj)) {
    return m_objectToColor[obj];
  } else {
    return glm::col4(0, 0, 0, 0);
  }
}

const void* Z3DPickingManager::objectOfColor(const glm::col4& col)
{
  if (col.a == 0) {
    return nullptr;
  }

  if (isRegistered(col)) {
    return m_colorToObject[col];
  } else {
    return nullptr;
  }
}

const void* Z3DPickingManager::objectAtWidgetPos(glm::ivec2 pos)
{
  assert(m_devicePixelRatio >= 1);
  pos[0] = pos[0] * m_devicePixelRatio;
  pos[1] = pos[1] * m_devicePixelRatio;

  // Vulkan path
  if (m_vkColor) {
    const int w = static_cast<int>(m_vkSize.x);
    const int h = static_cast<int>(m_vkSize.y);
    if (w <= 0 || h <= 0) {
      return nullptr;
    }
    // Clamp inside bounds
    pos.x = std::clamp(pos.x, 0, w - 1);
    pos.y = std::clamp(pos.y, 0, h - 1);
    const int yFlip = h - 1 - pos.y;
    // Prefer cached CPU buffer if available
    if (m_cachedColorValid && m_cachedColorSize.x == static_cast<uint32_t>(w) &&
        m_cachedColorSize.y == static_cast<uint32_t>(h) && !m_cachedColor.empty()) {
      const size_t idx = static_cast<size_t>(yFlip) * static_cast<size_t>(w) + static_cast<size_t>(pos.x);
      const uint8_t* rgba = &m_cachedColor[4 * idx];
      glm::col4 c{rgba[0], rgba[1], rgba[2], rgba[3]};
      return objectOfColor(c);
    }
    // Fallback to synchronous 1x1 download when cache not yet ready
    uint8_t rgba[4] = {0, 0, 0, 0};
    try {
      m_vkColor->downloadSubImage(rgba,
                                  sizeof(rgba),
                                  vk::Offset3D{pos.x, yFlip, 0},
                                  vk::Extent3D{1u, 1u, 1u},
                                  vk::ImageAspectFlagBits::eColor);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking color download failed: " << e.what();
      return nullptr;
    }
    glm::col4 c{rgba[0], rgba[1], rgba[2], rgba[3]};
    return objectOfColor(c);
  }

  if (!m_renderTarget) {
    return nullptr;
  }
  auto texSize = glm::ivec3(m_renderTarget->attachment(GL_COLOR_ATTACHMENT0)->dimension());
  pos[1] = texSize[1] - pos[1];
  return objectOfColor(m_renderTarget->colorAtPos(pos));
}

GLfloat Z3DPickingManager::depthAtWidgetPos(glm::ivec2 pos)
{
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
    const int yFlip = h - 1 - pos.y;

    const auto fmt = m_vkDepth->format();
    if (fmt == vk::Format::eD32Sfloat) {
      float val = 1.0f;
      try {
        m_vkDepth->downloadSubImage(&val,
                                    sizeof(val),
                                    vk::Offset3D{pos.x, yFlip, 0},
                                    vk::Extent3D{1u, 1u, 1u},
                                    vk::ImageAspectFlagBits::eDepth);
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "Vulkan picking depth download failed: " << e.what();
        return 1.0f;
      }
      return val;
    }
    // Default: D24UnormS8 (depth as 24-bit UNORM in low bits)
    uint32_t packed = 0u;
    try {
      m_vkDepth->downloadSubImage(&packed,
                                  sizeof(packed),
                                  vk::Offset3D{pos.x, yFlip, 0},
                                  vk::Extent3D{1u, 1u, 1u},
                                  vk::ImageAspectFlagBits::eDepth);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking depth download failed: " << e.what();
      return 1.0f;
    }
    return static_cast<float>(packed & 0x00FFFFFFu) / static_cast<float>(0x00FFFFFFu);
  }

  if (!m_renderTarget) {
    return 1.0f;
  }
  auto texSize = glm::ivec2(m_renderTarget->size());
  pos[1] = texSize[1] - pos[1];
  return m_renderTarget->depthAtPos(pos);
}

std::vector<const void*> Z3DPickingManager::sortObjectsByDistanceToPos(const glm::ivec2& pos, int radius, bool ascend)
{
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

  int w = 0;
  int h = 0;
  int baseY = 0;

  std::vector<uint8_t> rgba;
  std::unique_ptr<glm::col4[]> glBuf;

  if (m_vkColor) {
    w = static_cast<int>(m_vkSize.x);
    h = static_cast<int>(m_vkSize.y);
    if (w <= 0 || h <= 0) {
      return {};
    }
    // Clamp inside bounds (widget->physical coordinates).
    physPos.x = std::clamp(physPos.x, 0, w - 1);
    physPos.y = std::clamp(physPos.y, 0, h - 1);

    // Vulkan path uses a bottom-left origin in objectAtWidgetPos (via yFlip = h-1-y).
    baseY = (h - 1) - physPos.y;

    if (physRadius < 0) {
      physRadius = std::max(w, h);
    }

    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    try {
      m_vkColor->downloadData(rgba.data(), rgba.size());
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Vulkan picking color download failed: " << e.what();
      return {};
    }

    for (int yy = std::max(0, baseY - physRadius); yy <= std::min(h - 1, baseY + physRadius); ++yy) {
      for (int xx = std::max(0, physPos.x - physRadius); xx <= std::min(w - 1, physPos.x + physRadius); ++xx) {
        const size_t idx = static_cast<size_t>(yy) * static_cast<size_t>(w) + static_cast<size_t>(xx);
        const uint8_t* px = &rgba[4u * idx];
        const glm::col4 col{px[0], px[1], px[2], px[3]};
        recordColor(xx - physPos.x, yy - baseY, col);
      }
    }
  } else {
    CHECK(m_renderTarget);
    const Z3DTexture* tex = m_renderTarget->attachment(GL_COLOR_ATTACHMENT0);
    w = static_cast<int>(m_renderTarget->size().x);
    h = static_cast<int>(m_renderTarget->size().y);
    if (w <= 0 || h <= 0) {
      return {};
    }

    physPos.x = std::clamp(physPos.x, 0, w - 1);
    physPos.y = std::clamp(physPos.y, 0, h - 1);

    // GL path: match objectAtWidgetPos's y-flip convention.
    baseY = h - physPos.y;

    if (physRadius < 0) {
      physRadius = std::max(w, h);
    }

    const GLenum dataFormat = GL_BGRA;
    const GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;
    glBuf = std::make_unique_for_overwrite<glm::col4[]>(Z3DTexture::bypePerPixel(dataFormat, dataType) *
                                                        tex->numPixels() / 4);
    tex->downloadTextureToBuffer(dataFormat, dataType, glBuf.get());

    for (int yy = std::max(0, baseY - physRadius); yy <= std::min(h - 1, baseY + physRadius); ++yy) {
      for (int xx = std::max(0, physPos.x - physRadius); xx <= std::min(w - 1, physPos.x + physRadius); ++xx) {
        glm::col4 col = glBuf[(yy * w) + xx];
        std::swap(col.r, col.b);
        recordColor(xx - physPos.x, yy - baseY, col);
      }
    }
  }

  std::vector<const void*> res;
  if (ascend) {
    std::multimap<int, const void*> dist2obj;
    for (auto& [color, dist] : col2dist) {
      const void* obj = objectOfColor(color);
      if (obj) {
        dist2obj.emplace(dist, obj);
      }
    }
    for (auto& it : dist2obj) {
      res.push_back(it.second);
    }
  } else {
    std::multimap<int, const void*, std::greater<>> dist2obj;
    for (auto& [color, dist] : col2dist) {
      const void* obj = objectOfColor(color);
      if (obj) {
        dist2obj.emplace(dist, obj);
      }
    }
    for (auto& it : dist2obj) {
      res.push_back(it.second);
    }
  }
  return res;
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
  if (auto col = std::bit_cast<uint32_t>(m_currentColor); col != 0xffffffff) {
    ++col;
    m_currentColor = std::bit_cast<glm::col4>(col);
  } else {
    m_currentColor = glm::col4(0, 0, 0, 128);
    // LOG(ERROR) << "Out of colors...";
  }
}

} // namespace nim
