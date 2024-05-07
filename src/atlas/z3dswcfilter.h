#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zswcpack.h"
#include "zcolormap.h"
#include "zwidgetsgroup.h"
#include "z3dlinerenderer.h"
#include "z3dconerenderer.h"
#include "z3dsphererenderer.h"
#include "zeventlistenerparameter.h"
#include "zswccolorparameters.h"
#include <QObject>
#include <map>
#include <utility>
#include <vector>

namespace nim {

class Z3DSwcFilter : public Z3DGeometryFilter
{
  Q_OBJECT

public:
  enum class InteractionMode
  {
    Select,
    AddSwcNode,
    ConnectSwcNode,
    SmartExtendSwcNode
  };

  explicit Z3DSwcFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(ZSwcPack& swcPack);

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  bool isNodeRendering() const
  {
    return m_renderingPrimitive.isSelected("Sphere");
  }

  void setInteractionMode(InteractionMode mode)
  {
    m_interactionMode = mode;
  }

  InteractionMode interactionMode()
  {
    return m_interactionMode;
  }

  bool hasOpaque(Z3DEye) const override
  {
    return m_rendererBase.opacity() == 1.f && !m_renderingPrimitive.isSelected("Line");
  }

  void renderOpaque(Z3DEye eye) override;

  bool hasTransparent(Z3DEye) const override
  {
    return m_rendererBase.opacity() < 1.f || m_renderingPrimitive.isSelected("Line");
  }

  void renderTransparent(Z3DEye eye) override;

Q_SIGNALS:
  void treeNodeSelected(const ZSwc::SwcTreeNode*, bool append, bool extend);

  void connectingSwcTreeNode(ZSwc::SwcTreeNode);

  void treeNodeSelectConnection();

  void addNewSwcTreeNode(double x, double y, double z, double r);

  void extendSwcTreeNode(double x, double y, double z);

  void showSwcContextMenu(QPoint globalPos);

protected:
  void prepareColor();

  void selectSwc(QMouseEvent* e, int w, int h);

  void contextMenuEvent(QContextMenuEvent* e, int w, int h);

  double process(Z3DEye) override;

  void registerPickingObjects() override;

  void deregisterPickingObjects() override;

  void renderPicking(Z3DEye eye) override;

  void prepareData();

  void updateData();

  // get bounding box of swc tree in world coordinate
  void treeBound(ZSwcPack* swcPack, ZBBox<glm::dvec3>& result) const;

  // get bounding box of swc tree node in world coordinate
  void treeNodeBound(const ZSwc::ConstSwcTreeNode& tn, ZBBox<glm::dvec3>& result) const;

  void notTransformedTreeBound(ZSwcPack* swcPack, ZBBox<glm::dvec3>& result) const;

  // void updateAxisAlignedBoundBoxImpl() override;
  void updateNotTransformedBoundBoxImpl() override;

  void addSelectionLines() override;

  void addEditingSelectionLines() override;

  void notTransformedTreeNodeBound(const ZSwc::ConstSwcTreeNode& tn, ZBBox<glm::dvec3>& result) const;

private:
  static glm::dvec3 projectPointOnRay(const glm::dvec3& pt, const glm::dvec3& v1, const glm::dvec3& v2);

  void addSelectionBox(const std::pair<ZSwc::ConstSwcTreeNode, ZSwc::ConstSwcTreeNode>& nodePair,
                       std::vector<glm::vec3>& lines);

  void addSelectionBox(const ZSwc::ConstSwcTreeNode& tn, std::vector<glm::vec3>& lines);

  void deleteSelectedNodes();

private:
  Z3DLineRenderer m_lineRenderer;
  Z3DConeRenderer m_coneRenderer;
  Z3DSphereRenderer m_sphereRenderer;
  Z3DSphereRenderer m_sphereRendererForCone;

  ZStringIntOptionParameter m_renderingPrimitive;
  ZSwcColorParameters m_swcColorParameters;

  ZEventListenerParameter m_selectSwcEvent;
  ZEventListenerParameter m_deleteSelectedNodesEvent;
  ZEventListenerParameter m_contextMenuEvent;
  glm::ivec2 m_startCoord{};
  ZSwc* m_pressedSwc = nullptr;
  const ZSwc::SwcTreeNode* m_pressedSwcTreeNode = nullptr;

  std::vector<glm::vec4> m_baseAndBaseRadius;
  std::vector<glm::vec4> m_axisAndTopRadius;
  std::vector<glm::vec4> m_swcColors1;
  std::vector<glm::vec4> m_swcColors2;
  std::vector<glm::vec4> m_swcPickingColors;
  std::vector<glm::vec4> m_sphereForConePickingColors;
  std::vector<glm::vec3> m_lines;
  std::vector<glm::vec4> m_lineColors;
  std::vector<glm::vec4> m_linePickingColors;
  std::vector<glm::vec4> m_pointAndRadius;
  std::vector<glm::vec4> m_pointColors;
  std::vector<glm::vec4> m_pointPickingColors;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid;

  ZSwcPack* m_swcPack = nullptr;
  ZSwcPack* m_registeredSwcPack = nullptr;

  InteractionMode m_interactionMode;
};

} // namespace nim
