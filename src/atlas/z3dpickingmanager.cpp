#include "z3dpickingmanager.h"

#include "z3dgl.h"
#include "z3dtexture.h"
#include "zlog.h"
#include <functional>
#include <memory>
#include <vector>

namespace nim {

void Z3DPickingManager::setRenderTarget(Z3DRenderTarget& rt)
{
  CHECK(rt.attachment(GL_COLOR_ATTACHMENT0)->internalFormat() == GL_RGBA8);
  m_renderTarget = &rt;
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

  auto texSize = glm::ivec3(m_renderTarget->attachment(GL_COLOR_ATTACHMENT0)->dimension());
  pos[1] = texSize[1] - pos[1];
  return objectOfColor(m_renderTarget->colorAtPos(pos));
}

GLfloat Z3DPickingManager::depthAtWidgetPos(glm::ivec2 pos)
{
  assert(m_devicePixelRatio >= 1);
  pos[0] = pos[0] * m_devicePixelRatio;
  pos[1] = pos[1] * m_devicePixelRatio;

  auto texSize = glm::ivec2(m_renderTarget->size());
  pos[1] = texSize[1] - pos[1];
  return m_renderTarget->depthAtPos(pos);
}

std::vector<const void*> Z3DPickingManager::sortObjectsByDistanceToPos(const glm::ivec2& pos, int radius, bool ascend)
{
  boost::unordered_flat_map<glm::col4, int> col2dist;
  const Z3DTexture* tex = m_renderTarget->attachment(GL_COLOR_ATTACHMENT0);
  GLenum dataFormat = GL_BGRA;
  GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;
  auto buf =
    std::make_unique_for_overwrite<glm::col4[]>(Z3DTexture::bypePerPixel(dataFormat, dataType) * tex->numPixels() / 4);
  tex->downloadTextureToBuffer(dataFormat, dataType, buf.get());
  auto texSize = glm::ivec2(m_renderTarget->size());
  if (radius < 0) {
    radius = std::max(texSize.x, texSize.y);
  }
  for (auto y = std::max(0, pos.y - radius); y <= std::min(texSize.y - 1, pos.y + radius); ++y) {
    for (auto x = std::max(0, pos.x - radius); x <= std::min(texSize.x - 1, pos.x + radius); ++x) {
      auto col = buf[(y * texSize.x) + x];
      std::swap(col.r, col.b);
      if (col2dist[col] == 0) {
        col2dist[col] = (x - pos.x) * (x - pos.x) + (y - pos.y) * (y - pos.y);
      } else {
        col2dist[col] = std::min(col2dist[col], (x - pos.x) * (x - pos.x) + (y - pos.y) * (y - pos.y));
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
