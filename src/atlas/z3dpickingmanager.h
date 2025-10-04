#pragma once

#include "z3drendertarget.h"
#include "zglmutils.h"
#include <cstdint>
#include <map>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

namespace nim {

class ZVulkanTexture; // forward declare

class Z3DPickingManager
{
public:
  // input render target should have color internal format as GL_RGBA8
  // must call
  void setRenderTarget(Z3DRenderTarget& rt);

  // must call
  void setDevicePixelRatio(double dpr)
  {
    m_devicePixelRatio = dpr;
  }

  // Vulkan: set color/depth attachments for picking readback
  void setVulkanTargets(ZVulkanTexture* color, ZVulkanTexture* depth, const glm::uvec2& size)
  {
    m_vkColor = color;
    m_vkDepth = depth;
    m_vkSize = size;
  }

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

  void bindTarget()
  {
    m_renderTarget->bind();
  }

  void releaseTarget()
  {
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
};

} // namespace nim
