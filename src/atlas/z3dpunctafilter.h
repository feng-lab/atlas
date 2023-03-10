#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zwidgetsgroup.h"
#include "zcolormap.h"
#include "znumericparameter.h"
#include "z3dsphererenderer.h"
#include "zeventlistenerparameter.h"
#include "zpunctapack.h"
#include "z3dtextureglowrenderer.h"
#include "z3drenderport.h"
#include "z3dtexturecopyrenderer.h"
#include <QPoint>
#include <map>
#include <vector>

namespace nim {

class Z3DPunctaFilter : public Z3DGeometryFilter
{
  Q_OBJECT

public:
  explicit Z3DPunctaFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(ZPunctaPack& puncta);

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  // bool hasOpaque(Z3DEye eye) const override { return Z3DGeometryFilter::hasOpaque(eye) && !m_randomGlow.get(); }
  void renderOpaque(Z3DEye eye) override;

  // bool hasTransparent(Z3DEye eye) const override { return Z3DGeometryFilter::hasTransparent(eye) ||
  // m_randomGlow.get(); }
  void renderTransparent(Z3DEye eye) override;

Q_SIGNALS:

  void punctumSelected(const ZPunctum*, bool append);

protected:
  void prepareColor();

  void adjustWidgets();

  void changePunctaSize();

  void selectPuncta(QMouseEvent* e, int w, int h);

  void contextMenuEvent(QContextMenuEvent* e, int w, int h);

  void updateData();

  double process(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void registerPickingObjects() override;

  void deregisterPickingObjects() override;

  void prepareData();

  // result should have at least 6 elements
  void punctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const;

  // result should have at least 6 elements
  void notTransformedPunctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const;

  // void updateAxisAlignedBoundBoxImpl() override;
  void updateNotTransformedBoundBoxImpl() override;

  void addSelectionLines() override;

  void addEditingSelectionLines() override;

private:
  void deleteSelectedPuncta();

private:
  Z3DRenderOutputPort m_monoEyeOutport;
  Z3DRenderOutputPort m_leftEyeOutport;
  Z3DRenderOutputPort m_rightEyeOutport;
  Z3DRenderOutputPort m_monoEyeOutport2;
  Z3DRenderOutputPort m_leftEyeOutport2;
  Z3DRenderOutputPort m_rightEyeOutport2;

  Z3DSphereRenderer m_sphereRenderer;

  ZStringIntOptionParameter m_colorMode;
  ZVec4Parameter m_singleColorForAllPuncta;
  ZColorMapParameter m_colorMapScore;
  ZColorMapParameter m_colorMapMeanIntensity;
  ZColorMapParameter m_colorMapMaxIntensity;
  ZBoolParameter m_useSameSizeForAllPuncta;

  //  Z3DSphereRenderer m_glowSphereRenderer;
  //  Z3DTextureGlowRenderer m_textureGlowRenderer;
  //  ZBoolParameter m_randomGlow;
  //  ZFloatParameter m_glowPercentage;
  //  Z3DTextureCopyRenderer m_textureCopyRenderer;

  ZEventListenerParameter m_selectPunctumEvent;
  ZEventListenerParameter m_deleteSelectedPunctaEvent;
  ZEventListenerParameter m_contextMenuEvent;
  glm::ivec2 m_startCoord{};
  const ZPunctum* m_pressedPunctum = nullptr;

  std::vector<glm::vec4> m_pointAndRadius;
  std::vector<glm::vec4> m_specularAndShininess;
  std::vector<glm::vec4> m_pointColors;
  std::vector<glm::vec4> m_pointPickingColors;

  std::vector<glm::vec4> m_pointAndRadiusGlow;
  std::vector<glm::vec4> m_specularAndShininessGlow;
  std::vector<glm::vec4> m_pointColorsGlow;
  std::vector<glm::vec4> m_pointAndRadiusNormal;
  std::vector<glm::vec4> m_specularAndShininessNormal;
  std::vector<glm::vec4> m_pointColorsNormal;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid = false;

  ZPunctaPack* m_punctaPack = nullptr;
};

} // namespace nim
