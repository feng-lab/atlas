#ifndef Z3DPICKINGMANAGER_H
#define Z3DPICKINGMANAGER_H

#include "zglmutils.h"
#include <stdint.h>
#include <map>
#include "z3drendertarget.h"


namespace nim {

struct colComp
{
  // only work in little-endian system
  bool operator()(const glm::col4 &c1, const glm::col4 &c2) const
  {
    const uint32_t i1 = reinterpret_cast<const uint32_t&>(c1);
    const uint32_t i2 = reinterpret_cast<const uint32_t&>(c2);
    return i1 < i2;
  }
};

class Z3DPickingManager
{
public:
  // input render target should has color internal format as GL_RGBA8
  Z3DPickingManager();

  // must call
  void setRenderTarget(Z3DRenderTarget &rt);

  glm::col4 registerObject(const void* obj);
  void deregisterObject(const void* obj);
  void deregisterObject(const glm::col4& col);
  void clearRegisteredObjects();

  glm::col4 colorOfObject(const void* obj);
  glm::vec4 fColorOfObject(const void* obj) { return glm::vec4(glm::vec4(colorOfObject(obj)) / 255.f); }
  const void* objectOfColor(glm::col4 col);

  const void* objectAtWidgetPos(glm::ivec2 pos);

  // find all objects within a radius of pos, sort by distance
  // if radius is -1, search the whole image
  std::vector<const void*> sortObjectsByDistanceToPos(glm::ivec2 pos, int radius = -1, bool ascend = true);

  void bindTarget() { m_renderTarget->bind(); }
  void releaseTarget() { m_renderTarget->release(); }
  void clearTarget();

  Z3DRenderTarget& renderTarget() const { return *m_renderTarget; }

  bool isRegistered(const void* obj) { return m_objectToColor.find(obj) != m_objectToColor.end(); }
  bool isRegistered(glm::col4 col) { return m_colorToObject.find(col) != m_colorToObject.end(); }
private:
  void increaseColor();

  std::map<glm::col4, const void*, colComp> m_colorToObject;
  std::map<const void*, glm::col4> m_objectToColor;
  Z3DRenderTarget* m_renderTarget;
  glm::col4 m_currentColor;
};

} // namespace nim

#endif // Z3DPICKINGMANAGER_H
