#pragma once

#include "z3dfilter.h"
#include "z3dtransferfunction.h"
#include "z3dlinerenderer.h"
#include "z3dmeshrenderer.h"
#include "z3dsphererenderer.h"
#include "z3darrowrenderer.h"

namespace nim {

// child class should implement two pure virtual function and call updateBoundBox whenever its
// own parameter affect bound box
class Z3DBoundedFilter : public Z3DFilter
{
  Q_OBJECT

public:
  enum class BoundBoxRenderStyle
  {
    InheritState, // Do not modify GL state; caller is responsible
    OverlayAlphaDepth // Enable depth test + alpha blend locally
  };

  explicit Z3DBoundedFilter(Z3DGlobalParameters& globalPara, QObject* parent = nullptr);

  void setVisible(bool v)
  {
    m_visible.set(v);
  }

  [[nodiscard]] bool isVisible() const
  {
    return m_visible.get();
  }

  void setSelected(bool v);

  [[nodiscard]] bool isSelected() const
  {
    return m_isSelected;
  }

  [[nodiscard]] bool isTransformEnabled() const
  {
    return m_transformEnabled;
  }

  virtual void setViewport(glm::uvec2 viewport)
  {
    m_rendererBase.setViewport(viewport);
  }

  virtual void setViewport(glm::uvec4 viewport)
  {
    m_rendererBase.setViewport(viewport);
  }

  Z3DCamera& camera()
  {
    return m_rendererBase.camera();
  }

  Z3DCamera& globalCamera()
  {
    return m_rendererBase.globalCamera();
  }

  Z3DCameraParameter& globalCameraPara()
  {
    return m_rendererBase.globalCameraPara();
  }

  Z3DPickingManager& pickingManager()
  {
    return m_rendererBase.globalParas().pickingManager;
  }

  Z3DTrackballInteractionHandler& interactionHandler()
  {
    return m_rendererBase.globalParas().interactionHandler;
  }

  virtual void setShaderHookType(Z3DRendererBase::ShaderHookType t)
  {
    m_rendererBase.setShaderHookType(t);
  }

  virtual void setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t)
  {
    m_rendererBase.setShaderHookParaDDPDepthBlenderTexture(t);
  }

  virtual void setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t)
  {
    m_rendererBase.setShaderHookParaDDPFrontBlenderTexture(t);
  }

  Z3DRendererBase::ShaderHookParameter& shaderHookPara()
  {
    return m_rendererBase.shaderHookPara();
  }

  [[nodiscard]] const ZBBox<glm::dvec3>& axisAlignedBoundBox() const
  {
    return m_axisAlignedBoundBox;
  }

  [[nodiscard]] const ZBBox<glm::dvec3>& notTransformedBoundBox() const
  {
    return m_notTransformedBoundBox;
  }

  [[nodiscard]] glm::mat4 coordTransform() const
  {
    return m_rendererBase.coordTransform();
  }

  [[nodiscard]] glm::mat4 inverseCoordTransform() const
  {
    return m_rendererBase.inverseCoordTransform();
  }

  // Useful coordinate L->Left U->Up F->Front R->Right D->Down B->Back
  [[nodiscard]] glm::vec3 physicalLUF() const
  {
    return glm::vec3(m_notTransformedBoundBox.minCorner);
  }

  [[nodiscard]] glm::vec3 physicalRDB() const
  {
    return glm::vec3(m_notTransformedBoundBox.maxCorner);
  }

  [[nodiscard]] glm::vec3 physicalLDF() const
  {
    return glm::vec3(physicalLUF().x, physicalRDB().y, physicalLUF().z);
  }

  [[nodiscard]] glm::vec3 physicalRDF() const
  {
    return glm::vec3(physicalRDB().x, physicalRDB().y, physicalLUF().z);
  }

  [[nodiscard]] glm::vec3 physicalRUF() const
  {
    return glm::vec3(physicalRDB().x, physicalLUF().y, physicalLUF().z);
  }

  [[nodiscard]] glm::vec3 physicalLUB() const
  {
    return glm::vec3(physicalLUF().x, physicalLUF().y, physicalRDB().z);
  }

  [[nodiscard]] glm::vec3 physicalLDB() const
  {
    return glm::vec3(physicalLUF().x, physicalRDB().y, physicalRDB().z);
  }

  [[nodiscard]] glm::vec3 physicalRUB() const
  {
    return glm::vec3(physicalRDB().x, physicalLUF().y, physicalRDB().z);
  }

  // bound voxels in world coordinate
  [[nodiscard]] glm::vec3 worldLUF() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalLUF());
  }

  [[nodiscard]] glm::vec3 worldRDB() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalRDB());
  }

  [[nodiscard]] glm::vec3 worldLDF() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalLDF());
  }

  [[nodiscard]] glm::vec3 worldRDF() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalRDF());
  }

  [[nodiscard]] glm::vec3 worldRUF() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalRUF());
  }

  [[nodiscard]] glm::vec3 worldLUB() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalLUB());
  }

  [[nodiscard]] glm::vec3 worldLDB() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalLDB());
  }

  [[nodiscard]] glm::vec3 worldRUB() const
  {
    return glm::applyMatrix(m_rendererBase.coordTransform(), physicalRUB());
  }

  [[nodiscard]] virtual bool hasOpaque(Z3DEye) const
  {
    return m_rendererBase.opacity() == 1.f;
  }

  virtual void renderOpaque(Z3DEye) {}

  [[nodiscard]] virtual bool hasTransparent(Z3DEye) const
  {
    return m_rendererBase.opacity() < 1.f;
  }

  virtual void renderTransparent(Z3DEye) {}

  void renderHandle(Z3DEye eye);

  void renderHandlePicking(Z3DEye eye);

  void renderSelectionBox(Z3DEye eye);

  void renderEditingSelectionBox(Z3DEye eye);

  void rotateX() override;

  void rotateY() override;

  void rotateZ() override;

  void rotateXM() override;

  void rotateYM() override;

  void rotateZM() override;

  [[nodiscard]] ZBBox<glm::dvec3> axisAlignedBoundBoxAfterClipping() const;

  [[nodiscard]] ZBBox<glm::dvec3> notTransformedBoundBoxAfterClipping() const;

Q_SIGNALS:
  void boundBoxChanged();

  void objSelected(bool append);

  void objDeselected();

  void objVisibleChanged(bool v);

protected:
  void updateBoundBox();

  // implement this to empty function if clip planes are not needed
  virtual void setClipPlanes();

  void handleEvent(QMouseEvent* e, int w, int h);

  void initializeCutRange();

  void initializeRotationCenter();

  void setTransformEnabled(bool v)
  {
    m_transformEnabled = v;
  }

  void renderBoundBox(Z3DEye eye);

  // Optional state-aware variant; InheritState matches renderBoundBox(eye)
  void renderBoundBox(Z3DEye eye, BoundBoxRenderStyle style);

  static void appendBoundboxLines(const ZBBox<glm::dvec3>& bound, std::vector<glm::vec3>& lines);

  // output v1 is start point of ray, v2 is a point on the ray, v2-v1 is normalized
  // x and y are input screen point, width and height are input screen dimension
  void rayUnderScreenPoint(glm::vec3& v1, glm::vec3& v2, int x, int y, int width, int height);

  void rayUnderScreenPoint(glm::dvec3& v1, glm::dvec3& v2, int x, int y, int width, int height);

  // implement these to support bound box
  virtual void updateAxisAlignedBoundBoxImpl();

  virtual void updateNotTransformedBoundBoxImpl() {}

  // besides the big selection box, other additional lines can be added through this function
  virtual void addSelectionLines() {}

  // for select-and-editing
  virtual void addEditingSelectionLines() {}

  // reimplement this if cut range has different behavior
  virtual void expandCutRange();

private:
  void updateAxisAlignedBoundBox();

  void updateNotTransformedBoundBox();

  void onBoundBoxModeChanged();

  void updateBoundBoxLineColors();

  void updateSelectionLineColors();

  void onBoundBoxLineWidthChanged();

  void onSelectionBoundBoxLineWidthChanged();

  void invalidateHandle()
  {
    m_handleValid = false;
  }

  void makeSelectionGeometries();

  void updateHandle();

  void registerHandlePickingColors();

  int selectedHandle(const void* obj) const;

  void updateSelectedHandle(int handleIdx);

protected:
  Z3DRendererBase m_rendererBase;

  Z3DLineRenderer m_baseBoundBoxRenderer;
  Z3DLineRenderer m_selectionBoundBoxRenderer;
  Z3DMeshRenderer m_selectionCornerRenderer;
  Z3DSphereRenderer m_handleCenterRenderer;
  Z3DArrowRenderer m_handleArrowRenderer;

  ZBoolParameter m_visible;
  ZFloatSpanParameter m_xCut;
  ZFloatSpanParameter m_yCut;
  ZFloatSpanParameter m_zCut;
  ZStringIntOptionParameter m_boundBoxMode;
  ZIntParameter m_boundBoxLineWidth;
  ZVec4Parameter m_boundBoxLineColor;
  // Z3DTransferFunctionParameter m_boundBoxLineColor;
  ZIntParameter m_selectionLineWidth;
  ZVec4Parameter m_selectionLineColor;
  ZFloatParameter m_manipulatorSize;
  ZEventListenerParameter m_handleEvent;

  ZBBox<glm::dvec3> m_axisAlignedBoundBox;
  glm::vec3 m_center{};
  ZBBox<glm::dvec3> m_notTransformedBoundBox;

  std::vector<glm::vec3> m_normalBoundBoxLines;
  std::vector<glm::vec3> m_axisAlignedBoundBoxLines;
  std::vector<glm::vec4> m_boundBoxLineColors;

  std::vector<glm::vec3> m_selectionLines; // selection lines for the whole object
  ZMesh m_selectionCornerCubes;
  std::vector<ZMesh*> m_selectionCornerCubesWrapper;
  std::vector<glm::vec4> m_selectionLineColors;
  std::vector<glm::vec3> m_editingSelectionLines; // selection lines for select-and-editing

  std::vector<glm::vec4> m_handleCenterAndRadius;
  std::vector<glm::vec4> m_handleCenterColors;
  std::vector<glm::vec4> m_handleCenterPickingColors;
  std::vector<glm::vec4> m_handleArrowTailPosAndTailRadius;
  std::vector<glm::vec4> m_handleArrowheadPosAndHeadRadius;
  std::vector<glm::vec4> m_handleArrowColors;
  std::vector<glm::vec4> m_handleArrowPickingColors;

  bool m_canUpdateClipPlane;

  bool m_isSelected;

private:
  ZBBox<glm::dvec3> m_selectionBoundBox;

  glm::ivec2 m_lastMousePosition{};
  glm::vec3 m_startMouseWorldPos{};
  glm::vec3 m_startTrans{};
  float m_startDepth{};
  int m_selectedHandle;

  bool m_transformEnabled;

  bool m_handleValid;
};

} // namespace nim
