#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zskeleton.h"
#include "zwidgetsgroup.h"
#include "z3dconerenderer.h"
#include "z3dlinerenderer.h"
#include "z3dsphererenderer.h"

#include <QObject>
#include <vector>

namespace nim {

class Z3DSkeletonFilter : public Z3DGeometryFilter
{
  Q_OBJECT

public:
  explicit Z3DSkeletonFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(ZSkeleton& skeleton);

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  [[nodiscard]] bool hasOpaque(Z3DEye eye) const override
  {
    return Z3DGeometryFilter::hasOpaque(eye) && !m_renderingPrimitive.isSelected("Line");
  }

  void renderOpaque(Z3DEye eye) override;

  [[nodiscard]] bool hasTransparent(Z3DEye eye) const override
  {
    return Z3DGeometryFilter::hasTransparent(eye) || m_renderingPrimitive.isSelected("Line");
  }

  void renderTransparent(Z3DEye eye) override;

protected:
  double process(Z3DEye eye) override;

  void updateNotTransformedBoundBoxImpl() override;

private:
  void prepareData();

  void prepareColor();

private:
  Z3DLineRenderer m_lineRenderer;
  Z3DConeRenderer m_coneRenderer;
  Z3DSphereRenderer m_sphereRenderer;

  ZStringIntOptionParameter m_renderingPrimitive;
  ZVec4Parameter m_color;

  std::vector<glm::vec3> m_lines;
  std::vector<glm::vec4> m_lineColors;

  std::vector<glm::vec4> m_baseAndBaseRadius;
  std::vector<glm::vec4> m_axisAndTopRadius;
  std::vector<glm::vec4> m_coneColors;

  std::vector<glm::vec4> m_pointAndRadius;
  std::vector<glm::vec4> m_pointColors;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid = false;

  ZSkeleton* m_skeleton = nullptr;
};

} // namespace nim

