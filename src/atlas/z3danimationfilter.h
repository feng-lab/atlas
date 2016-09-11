#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "z3danimation.h"
#include "zcameraparameteranimation.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "z3darrowrenderer.h"
#include "z3dlinerenderer.h"
#include "z3dmeshrenderer.h"
#include "zcolormap.h"
#include "zmesh.h"
#include <QString>
#include <QPoint>
#include <map>
#include <vector>

namespace nim {

class Z3DAnimationFilter : public Z3DGeometryFilter
{
Q_OBJECT
public:
  explicit Z3DAnimationFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setVisible(bool v)
  { m_visible.set(v); }

  virtual void process(Z3DEye /*unused*/) override;

  void setData(Z3DAnimation* animation);

  virtual bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  virtual bool hasOpaque(Z3DEye /*unused*/) const override
  { return false; }

  virtual void renderOpaque(Z3DEye eye) override;

  virtual bool hasTransparent(Z3DEye /*unused*/) const override
  { return true; }

  virtual void renderTransparent(Z3DEye eye) override;

protected:
  void prepareColor();

  void setClipPlanes() override;

  void updateData();

  void adjustWidgets();

  void updateLineWidth();

  virtual void renderPicking(Z3DEye eye) override;

  void prepareData();

  virtual void registerPickingObjects() override;

  virtual void deregisterPickingObjects() override;

  virtual void updateNotTransformedBoundBoxImpl() override;

private:
  const ZCameraParameterAnimation* cameraParaAnimation() const
  { return m_animation->cameraParameterAnimation(); }

  ZCameraParameterAnimation* cameraParaAnimation()
  { return m_animation->cameraParameterAnimation(); }

private:
  Z3DLineRenderer m_lineRenderer;
  Z3DArrowRenderer m_arrowRenderer;
  Z3DMeshRenderer m_triangleListRenderer;

  bool m_dataIsInvalid;
  Z3DAnimation* m_animation;
  ZBoolParameter m_visible;
  ZIntParameter m_lineWidth;
  ZStringIntOptionParameter m_colorMode;
  ZVec4Parameter m_color;
  ZColorMapParameter m_colorMap;
  ZDoubleParameter m_timeInterval;
  ZFloatParameter m_cameraSize;
  ZBoolParameter m_showCameraDirection;
  ZFloatParameter m_cameraDirectionSize;
  ZVec4Parameter m_upDirectionColor;
  ZVec4Parameter m_viewDirectionColor;
  ZDoubleParameter m_cameraDirectionTimeInterval;
  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;

  std::vector<glm::vec3> m_lines;
  std::vector<glm::vec4> m_lineColors;
  std::vector<double> m_times;
  std::vector<glm::vec4> m_tailPosAndTailRadius;
  std::vector<glm::vec4> m_headPosAndHeadRadius;
  std::vector<glm::vec4> m_arrowColors;
  std::vector<double> m_cameraDirectionTimes;
  ZMesh m_triangles;
  std::vector<ZMesh*> m_trianglesWrapper;

  bool m_locked;
};

} // namespace nim

