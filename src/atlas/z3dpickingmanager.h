#pragma once

#include "z3drendertarget.h"
#include "zglmutils.h"
#include <cstdint>
#include <map>
#include <cstring>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

namespace nim {

class ZVulkanTexture; // forward declare

class Z3DPickingManager
{
public:
  // Input render target should have color internal format GL_RGBA8.
  void setPickingTarget(Z3DRenderTarget& rt);
  void setPickingTarget(ZVulkanTexture& color,
                        ZVulkanTexture& depth,
                        const glm::uvec2& size);

  // must call
  void setDevicePixelRatio(double dpr)
  {
    m_devicePixelRatio = dpr;
  }

  void resetRenderTarget();

  glm::col4 registerObject(const void* obj);

  void deregisterObject(const void* obj);

  void deregisterObject(const glm::col4& col);

  void clearRegisteredObjects();

  glm::col4 colorOfObject(const void* obj);

  glm::vec4 fColorOfObject(const void* obj)
  {
    return glm::vec4(glm::vec4(colorOfObject(obj)) / 255.f);
  }

  const void* objectOfColor(const glm::col4& col);

  const void* objectAtWidgetPos(glm::ivec2 pos);

  // Depth at widget pixel (accounts for devicePixelRatio and y-flip)
  GLfloat depthAtWidgetPos(glm::ivec2 pos);

  // find all objects within a radius of pos, sort by distance
  // if radius is -1, search the whole image
  std::vector<const void*> sortObjectsByDistanceToPos(const glm::ivec2& pos, int radius = -1, bool ascend = true);

  bool isHit(const glm::ivec2& pos, const void* obj)
  {
    return (objectAtWidgetPos(pos) == obj);
  }

  [[nodiscard]] bool hasGlTarget() const
  {
    return m_renderTarget != nullptr;
  }

  [[nodiscard]] bool hasVulkanTarget() const
  {
    return m_vkColor != nullptr && m_vkDepth != nullptr;
  }

  void bindTarget()
  {
    CHECK(m_renderTarget != nullptr) << "Attempted to bind picking target before it was set";
    m_renderTarget->bind();
  }

  void releaseTarget()
  {
    CHECK(m_renderTarget != nullptr) << "Attempted to release picking target before it was set";
    m_renderTarget->release();
  }

  static void clearTarget();

  [[nodiscard]] Z3DRenderTarget& renderTarget() const
  {
    return *m_renderTarget;
  }

  bool isRegistered(const void* obj)
  {
    return m_objectToColor.contains(obj);
  }

  bool isRegistered(const glm::col4& col)
  {
    return m_colorToObject.contains(col);
  }

private:
  void increaseColor();
  void clearVulkanState();

private:
  boost::unordered_flat_map<glm::col4, const void*> m_colorToObject;
  boost::unordered_flat_map<const void*, glm::col4> m_objectToColor;
  Z3DRenderTarget* m_renderTarget = nullptr;
  glm::col4 m_currentColor{0, 0, 0, 128};
  double m_devicePixelRatio = 1.;

  // Vulkan attachments for picking (optional)
  ZVulkanTexture* m_vkColor = nullptr;
  ZVulkanTexture* m_vkDepth = nullptr;
  glm::uvec2 m_vkSize{0u, 0u};

  // Cached CPU copy of Vulkan picking color buffer (latest ready frame)
  std::vector<uint8_t> m_cachedColor;
  glm::uvec2 m_cachedColorSize{0u, 0u};
  bool m_cachedColorValid = false;

public:
  // Update cached picking color buffer (RGBA8) from a CPU pointer.
  // Copies data; safe to call from the rendering thread after fence.
  // data may be null to clear the cache
  void updateCachedVulkanPickingColor(/*nullable*/ const uint8_t* data, size_t bytes, glm::uvec2 size)
  {
    if (!data || size.x == 0u || size.y == 0u || bytes < static_cast<size_t>(size.x) * size.y * 4u) {
      m_cachedColorValid = false;
      m_cachedColor.clear();
      m_cachedColorSize = glm::uvec2(0u);
      return;
    }
    m_cachedColor.resize(static_cast<size_t>(size.x) * size.y * 4u);
    std::memcpy(m_cachedColor.data(), data, m_cachedColor.size());
    m_cachedColorSize = size;
    m_cachedColorValid = true;
  }
};

} // namespace nim
