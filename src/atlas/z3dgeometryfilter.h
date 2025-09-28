#pragma once

#include "z3dboundedfilter.h"
#include "z3dpickingmanager.h"
#include "z3dport.h"
#include "z3drendererbase.h"

namespace nim {

class Z3DGeometryFilter : public Z3DBoundedFilter
{
  friend class Z3DCompositor;

public:
  explicit Z3DGeometryFilter(Z3DGlobalParameters& globalPara, QObject* parent = nullptr);

  virtual void renderPicking(Z3DEye) {}

  [[nodiscard]] bool isStayOnTop() const
  {
    return m_stayOnTop.get();
  }

  void setStayOnTop(bool s)
  {
    m_stayOnTop.set(s);
  }

  [[nodiscard]] float opacity() const
  {
    return m_rendererParameters.opacity.get();
  }

  void setOpacity(float v)
  {
    m_rendererParameters.opacity.set(v);
  }

  [[nodiscard]] float sizeScale() const
  {
    return m_rendererParameters.sizeScale.get();
  }

  void setSizeScale(float s)
  {
    m_rendererParameters.sizeScale.set(s);
  }

protected:
  double process(Z3DEye) override
  {
    syncRendererState();
    return 1.0;
  }

  // once processed, should be valid for both stereo view and mono view
  void setValid(Z3DEye eye) override
  {
    Z3DBoundedFilter::setValid(eye);
    m_state = State::Valid;
  }

  // functions for picking, use these two function and m_pickingObjectsRegistered to control picking
  // note: input Z3DPickingManager might be nullptr
  // after deregister, m_pickingObjectsRegistered should be false;
  virtual void deregisterPickingObjects() {}

  // after register, m_pickingObjectsRegistered should be true and data picking color should be set
  // for renderers
  virtual void registerPickingObjects() {}

protected:
  Z3DFilterOutputPort<Z3DGeometryFilter> m_outPort;

  ZBoolParameter m_stayOnTop;

  Z3DPickingManager* m_pickingManager = nullptr;
  bool m_pickingObjectsRegistered;
};

} // namespace nim
