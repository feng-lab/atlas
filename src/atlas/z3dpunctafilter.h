#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zwidgetsgroup.h"
#include "zcolormap.h"
#include "znumericparameter.h"
#include "z3dsphererenderer.h"
#include "zeventlistenerparameter.h"
#include "zpuncta.h"
#include "z3dtextureglowrenderer.h"
#include "z3drenderport.h"
#include "z3dtexturecopyrenderer.h"
#include <QString>
#include <QPoint>
#include <map>
#include <vector>

namespace nim {

class Z3DPunctaFilter : public Z3DGeometryFilter
{
Q_OBJECT
public:
  explicit Z3DPunctaFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(ZPuncta& puncta);

  inline void setSelectedPuncta(std::set<ZPunctum*>* list)
  { m_selectedPuncta = list; }

  virtual bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  inline void setColorMode(const std::string& mode)
  {
    m_colorMode.select(mode.c_str());
  }

  //virtual bool hasOpaque(Z3DEye eye) const override { return Z3DGeometryFilter::hasOpaque(eye) && !m_randomGlow.get(); }
  virtual void renderOpaque(Z3DEye eye) override;

  //virtual bool hasTransparent(Z3DEye eye) const override { return Z3DGeometryFilter::hasTransparent(eye) || m_randomGlow.get(); }
  virtual void renderTransparent(Z3DEye eye) override;

signals:

  void punctumSelected(ZPunctum*, bool append);

protected:
  void prepareColor();

  void adjustWidgets();

  void changePunctaSize();

  void selectPuncta(QMouseEvent* e, int w, int h);

  void updateData();

  virtual void process(Z3DEye eye) override;

  virtual void renderPicking(Z3DEye eye) override;

  virtual void registerPickingObjects() override;

  virtual void deregisterPickingObjects() override;

  void prepareData();

  // result should have at least 6 elements
  void punctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const;

  // result should have at least 6 elements
  void notTransformedPunctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const;

  //virtual void updateAxisAlignedBoundBoxImpl() override;
  virtual void updateNotTransformedBoundBoxImpl() override;

  virtual void addSelectionLines() override;

private:
  // get visible data from origPunctaList put into punctaList
  void getVisibleData();

  void updatePunctumVisibleState();

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

  //std::map<QString, size_t> m_sourceColorMapper;   // should use unordered_map
  // puncta list used for rendering, it is a subset of m_origPunctaList. Some puncta are
  // hidden because they are unchecked from the object model. This allows us to control
  // the visibility of each single punctum.
  std::vector<ZPunctum*> m_punctaList;
  std::vector<ZPunctum*> m_registeredPunctaList;    // used for picking

  ZEventListenerParameter m_selectPunctumEvent;
  glm::ivec2 m_startCoord;
  ZPunctum* m_pressedPunctum = nullptr;
  std::set<ZPunctum*>* m_selectedPuncta = nullptr;   //point to all selected puncta, managed by other class

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

  ZPuncta* m_origPuncta = nullptr;
};

} // namespace nim

