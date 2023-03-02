#pragma once

#include "z3drendertarget.h"
#include "zglmutils.h"
#include <cstdint>
#include <map>
#include <vector>

namespace nim {

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

  Z3DRenderTarget& renderTarget() const
  {
    return *m_renderTarget;
  }

  bool isRegistered(const void* obj)
  {
    return m_objectToColor.find(obj) != m_objectToColor.end();
  }

  bool isRegistered(const glm::col4& col)
  {
    return m_colorToObject.find(col) != m_colorToObject.end();
  }

private:
  void increaseColor();

private:
  std::map<glm::col4, const void*, Col4Compare> m_colorToObject;
  std::map<const void*, glm::col4> m_objectToColor;
  Z3DRenderTarget* m_renderTarget = nullptr;
  glm::col4 m_currentColor{0, 0, 0, 128};
  double m_devicePixelRatio = 1.;
};

} // namespace nim
