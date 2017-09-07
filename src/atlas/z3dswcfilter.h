#pragma once

#include "z3dgeometryfilter.h"
#include "zoptionparameter.h"
#include "zswc.h"
#include "zcolormap.h"
#include "zwidgetsgroup.h"
#include "z3dlinerenderer.h"
#include "z3dconerenderer.h"
#include "z3dsphererenderer.h"
#include "zeventlistenerparameter.h"
#include <QObject>
#include <QString>
#include <map>
#include <utility>
#include <vector>

namespace nim {

class Z3DSwcFilter : public Z3DGeometryFilter
{
Q_OBJECT
  using SwcTreeNode = ZSwc::Iterator;
public:
  enum class InteractionMode
  {
    Select, AddSwcNode, ConnectSwcNode, SmartExtendSwcNode
  };

  explicit Z3DSwcFilter(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  void setData(ZSwc& tree);

  inline void setSelectedSwcs(std::set<ZSwc*>* list)
  {
    m_selectedSwcs = list;
  }

  inline void setSelectedSwcTreeNodes(std::set<SwcTreeNode>* list)
  {
    m_selectedSwcTreeNodes = list;
  }

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> widgetsGroup();

  inline void setRenderingPrimitive(const std::string& mode)
  {
    m_renderingPrimitive.select(mode.c_str());
  }

  bool isNodeRendering() const
  { return m_renderingPrimitive.isSelected("Sphere"); }

  void setInteractionMode(InteractionMode mode)
  { m_interactionMode = mode; }

  inline InteractionMode interactionMode()
  { return m_interactionMode; }

  bool hasOpaque(Z3DEye /*unused*/) const override
  { return m_rendererBase.opacity() == 1.f && !m_renderingPrimitive.isSelected("Line"); }

  void renderOpaque(Z3DEye eye) override;

  bool hasTransparent(Z3DEye /*unused*/) const override
  { return m_rendererBase.opacity() < 1.f || m_renderingPrimitive.isSelected("Line"); }

  void renderTransparent(Z3DEye eye) override;

signals:

  void treeSelected(ZSwc*, bool append);

  void treeNodeSelected(SwcTreeNode, bool append);

  void connectingSwcTreeNode(SwcTreeNode);

  void treeNodeSelectConnection();

  void addNewSwcTreeNode(double x, double y, double z, double r);

  void extendSwcTreeNode(double x, double y, double z);

protected:
  void prepareColor();

  void adjustWidgets();

  void selectSwc(QMouseEvent* e, int w, int h);

  void setColorMode(const std::string& mode);

  void process(Z3DEye /*unused*/) override;

  void registerPickingObjects() override;

  void deregisterPickingObjects() override;

  void renderPicking(Z3DEye eye) override;

  void prepareData();

  //get bounding box of swc tree in world coordinate
  void treeBound(ZSwc* tree, ZBBox<glm::dvec3>& result) const;

  //get bounding box of swc tree node in world coordinate
  void treeNodeBound(const SwcTreeNode& tn, ZBBox<glm::dvec3>& result) const;

  void notTransformedTreeBound(ZSwc* tree, ZBBox<glm::dvec3>& result) const;

  //void updateAxisAlignedBoundBoxImpl() override;
  void updateNotTransformedBoundBoxImpl() override;

  void addSelectionLines() override;

  void notTransformedTreeNodeBound(const SwcTreeNode& tn, ZBBox<glm::dvec3>& result) const;

private:
  void initTopologyColor();

  void initTypeColor();

  void initSubclassTypeColor();

  void decompseSwcTree();

  glm::vec4 colorByType(const SwcTreeNode& n);

  glm::vec4 colorByDirection(const SwcTreeNode& n);

  glm::dvec3 projectPointOnRay(
    const glm::dvec3& pt, const glm::dvec3& v1, const glm::dvec3& v2);

  void addSelectionBox(const std::pair<SwcTreeNode, SwcTreeNode>& nodePair,
                       std::vector<glm::vec3>& lines);

  void addSelectionBox(const SwcTreeNode& tn, std::vector<glm::vec3>& lines);

private:
  Z3DLineRenderer m_lineRenderer;
  Z3DConeRenderer m_coneRenderer;
  Z3DSphereRenderer m_sphereRenderer;
  Z3DSphereRenderer m_sphereRendererForCone;

  ZStringIntOptionParameter m_renderingPrimitive;
  ZStringIntOptionParameter m_colorMode;
  ZVec4Parameter m_swcTreeColor;
  std::vector<std::unique_ptr<ZVec4Parameter>> m_colorsForDifferentType;
  std::vector<std::unique_ptr<ZVec4Parameter>> m_colorsForSubclassType;
  std::map<int, size_t> m_subclassTypeColorMapper;
  std::vector<std::unique_ptr<ZVec4Parameter>> m_colorsForDifferentTopology;
  ZColorMapParameter m_colorMapBranchType;

  //std::map<std::string, size_t> m_sourceColorMapper;   // should use unordered_map
  // swc list used for rendering, it is a subset of m_origSwcList. Some swcs are
  // hidden because they are unchecked from the object model. This allows us to control
  // the visibility of each single swc tree.
  std::vector<SwcTreeNode*> m_registeredSwcTreeNodeList;    // used for picking

  ZEventListenerParameter m_selectSwcEvent;
  glm::ivec2 m_startCoord;
  ZSwc* m_pressedSwc;
  std::set<ZSwc*>* m_selectedSwcs;   //point to all selected swcs, managed by other class
  SwcTreeNode* m_pressedSwcTreeNode;
  std::set<SwcTreeNode>* m_selectedSwcTreeNodes;   //point to all selected swcs, managed by other class

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

  std::vector<std::pair<SwcTreeNode, SwcTreeNode>> m_decompsedNodePairs;
  std::vector<SwcTreeNode> m_decomposedNodes;
  std::set<SwcTreeNode*> m_allNodesSet;  // for fast search
  std::set<int> m_allNodeType;   // all node type of current opened swc, used for adjust widget (hide irrelavant stuff)

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_dataIsInvalid;

  ZSwc* m_swcTree;
  ZSwc* m_registeredSwcTree;

  InteractionMode m_interactionMode;
};

} // namespace nim

