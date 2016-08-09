#pragma once

#include "z3dboundedfilter.h"
#include "z3drendererbase.h"
#include "z3dport.h"
#include "z3dpickingmanager.h"

namespace nim {

class Z3DGeometryFilter : public Z3DBoundedFilter
{
  friend class Z3DCompositor;

public:
  Z3DGeometryFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  virtual void renderPicking(Z3DEye)
  {}

  bool isStayOnTop() const
  { return m_stayOnTop.get(); }

  void setStayOnTop(bool s)
  { m_stayOnTop.set(s); }

  glm::mat4 coordTransform() const
  { return m_rendererBase.coordTransform(); }

  float opacity() const
  { return m_rendererBase.opacity(); }

  void setOpacity(float v)
  { m_rendererBase.setOpacity(v); }

  float sizeScale() const
  { return m_rendererBase.sizeScale(); }

  void setSizeScale(float s)
  { m_rendererBase.setSizeScale(s); }

protected:
  virtual void process(Z3DEye) override
  {}

  // once processed, should be valid for both stereo view and mono view
  virtual void setValid(Z3DEye eye) override
  {
    Z3DBoundedFilter::setValid(eye);
    m_invalidationState = Valid;
  }

  // functions for picking, use these two function and m_pickingObjectsRegistered to control picking
  // note: input Z3DPickingManager might be nullptr
  // after deregister, m_pickingObjectsRegistered should be false;
  virtual void deregisterPickingObjects()
  {}

  // after register, m_pickingObjectsRegistered should be true and data picking color should be set
  // for renderers
  virtual void registerPickingObjects()
  {}

protected:
  Z3DFilterOutputPort<Z3DGeometryFilter> m_outPort;

  ZBoolParameter m_stayOnTop;

  Z3DPickingManager* m_pickingManager;
  bool m_pickingObjectsRegistered;
};

} // namespace nim

