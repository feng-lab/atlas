#pragma once

#include "zobjpack.h"
#include "zswc.h"
#include "zglmutils.h"
#include "zbbox.h"
#include <QUndoStack>
#include <QMenu>
#include <QAction>
#include <utility>
#include <vector>
#include <set>
#include <cstdint>

namespace nim {

class ZSwcDoc;

class ZSwcPack : public ZObjPack
{
  Q_OBJECT

public:
  ZSwcPack(ZSwc swc, const QString& path, size_t id, ZSwcDoc& pd, QObject* parent = nullptr);

  ~ZSwcPack() override;

  void updateDerivedData();

  const QString& info() const;

  const QString& name() const
  {
    return m_name;
  }

  const QString& tooltip() const
  {
    return m_tooltip;
  }

  const QString& path() const
  {
    return m_path;
  }

  QUndoStack* undoStack()
  {
    return &m_undoStack;
  }

  QMenu& contextMenu();

  void save(const QString& fileName);

  // void setSelectedPuncta(const std::set<const ZPunctum*>& sp);

  const ZSwc& swc() const
  {
    return m_swc;
  }

  [[nodiscard]] ZSwc::SwcTreeNode findNodeByIdOrNull(int64_t id);

  // Replace the underlying SWC tree as a single undoable edit.
  //
  // This is intended for operations (like tracing) that compute a new tree state off the UI thread and then apply it to
  // the document model in one step.
  void replaceSwcWithUndo(const QString& undoText, ZSwc newSwc);

  ZBBox<glm::ivec4> boundBox() const;

  const std::vector<ZSwc::SwcTreeNode>& rootNodes() const
  {
    return m_rootNodes;
  }

  const std::map<ZSwc::SwcTreeNode, std::vector<ZSwc::SwcTreeNode>>& rootToChildrenNodes() const
  {
    return m_rootToChildrenNodes;
  }

  const std::set<ZSwc::SwcTreeNode>& selectedNodes() const
  {
    return m_selectedNodes;
  }

  const std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>>& decompsedNodePairs() const
  {
    return m_decompsedNodePairs;
  }

  const std::vector<ZSwc::SwcTreeNode>& decomposedNodes() const
  {
    return m_decomposedNodes;
  }

  const std::set<const ZSwc::SwcTreeNode*>& allNodesSet() const
  {
    return m_allNodesSet;
  }

  const std::set<int>& allNodeType() const
  {
    return m_allNodeType;
  }

  void setSelectedNodes(const std::set<ZSwc::SwcTreeNode>& sn);

  std::tuple<int, int> getParentRowAndRowOfNode(const ZSwc::SwcTreeNode& node) const;

  ZSwc::SwcTreeNode getNodeOfParentRowAndRow(const std::tuple<int, int>& prar) const;

  void onTreeNodeSelected(const ZSwc::SwcTreeNode* p, bool append, bool extend);

  // neuTube context menu (2D/3D): extend from the single selected node by adding a new child at `center`.
  // Selection behavior matches neuTube: the newly created node becomes the only selected node.
  void extendSelectedNodePlain(const glm::dvec3& center, double radius);

  // neuTube context menu (2D/3D): smart extend from the single selected node by computing a path to `center`
  // using the configured source image/channel. This matches `ZStackDoc::executeSwcNodeSmartExtendCommand(...)`.
  void extendSelectedNodeSmartLegacyLike(const glm::dvec3& center, double radius, size_t t);

  // neuTube: Add an isolated neuron node (as a new root) at `center`.
  void addIsolatedNodeLegacyLike(const glm::dvec3& center, double radius);

  // neuTube: "Move to Current Plane" (2D) sets Z of all selected nodes to the current slice.
  void setSelectedNodesZLegacyLike(double z);

  // neuTube: Move selected SWC nodes by an offset (used by the 2D/3D move modes).
  void translateSelectedNodesLegacyLike(double dx, double dy, double dz);

  // neuTube context menu (2D/3D): connect the single selected node (anchor) to `target`.
  // This matches legacy `ZStackDoc::executeConnectSwcNodeCommand(anchor, target)` semantics:
  // - Re-roots the target tree at `target`, then attaches it as a child of the anchor.
  // - Does nothing when the nodes are already connected (same tree).
  // - Does not change the current selection.
  [[nodiscard]] bool connectSelectedNodeToLegacyLike(const ZSwc::SwcTreeNode& target);

  void deleteSelectedNodes();

  void deleteUnselectedNodes();

  void mergeSelectedNodes();

  void insertNodesBetweenSelectedPairs();

  void interpolateSelectedNodes();

  void interpolateSelectedNodesZ();

  void interpolateSelectedNodesPosition();

  void interpolateSelectedNodesRadius();

  void showSwcSummary();

  void showSelectedBranchLength();

  void showSelectedBranchScaledLength();

  void showSwcContextMenu(QPoint globalPos);

protected:
  void updateViewRelatedData();

  void createContextMenu();

  void selectDownstreamNodes();

  void selectUpstreamNodes();

  void selectNeighborNodes();

  void selectHostBranchNodes();

  void selectConnectedNodes();

  void selectAllNodes();

  void setSelectedNodeAsRoot();

  void changeSelectedNodeType();

  void translateSelectedNodes();

  void changeSelectedNodeSize();

  void removeTurn();

  void resolveCrossover();

  void joinIsolatedBranch();

  void joinIsolatedBranchAcrossTrees();

  void resetBranchPoint();

  void breakSelectedNodes();

  void connectSelectedNodes();

Q_SIGNALS:
  void selectionChanged();

  void swcChanged();

  void undoStackCleanChanged(bool clean);

protected:
  ZSwc m_swc;
  QString m_path;
  ZSwcDoc& m_doc;
  QUndoStack m_undoStack;

  QAction* m_selectDownstreamAction = nullptr;
  QAction* m_selectUpstreamAction = nullptr;
  QAction* m_selectNeighborAction = nullptr;
  QAction* m_selectHostBranchAction = nullptr;
  QAction* m_selectConnectedAction = nullptr;
  QAction* m_selectAllAction = nullptr;
  QAction* m_deleteSelectedNodesAction = nullptr;
  QAction* m_deleteUnselectedNodesAction = nullptr;
  QAction* m_setSelectedNodeAsRootAction = nullptr;
  QAction* m_changeSelectedNodeTypeAction = nullptr;
  QAction* m_translateSelectedNodesAction = nullptr;
  QAction* m_changeSelectedNodeSizeAction = nullptr;
  QAction* m_breakSelectedNodesAction = nullptr;
  QAction* m_connectSelectedNodesAction = nullptr;
  QAction* m_mergeSelectedNodesAction = nullptr;
  QAction* m_insertNodeAction = nullptr;
  QAction* m_interpolateAction = nullptr;
  QAction* m_interpolateZAction = nullptr;
  QAction* m_interpolatePositionAction = nullptr;
  QAction* m_interpolateRadiusAction = nullptr;
  QAction* m_showSummaryAction = nullptr;
  QAction* m_showSelectedBranchLengthAction = nullptr;
  QAction* m_showSelectedBranchScaledLengthAction = nullptr;
  QAction* m_removeTurnAction = nullptr;
  QAction* m_resolveCrossoverAction = nullptr;
  QAction* m_joinIsolatedBranchAction = nullptr;
  QAction* m_joinIsolatedBranchAcrossTreesAction = nullptr;
  QAction* m_resetBranchPointAction = nullptr;
  QMenu m_contextMenu;

  // derived data

private:
  friend class ZSwcEditCommand;

  mutable QString m_info;
  QString m_name;
  QString m_tooltip;
  // for views
  std::vector<ZSwc::SwcTreeNode> m_rootNodes;
  std::map<ZSwc::SwcTreeNode, std::vector<ZSwc::SwcTreeNode>> m_rootToChildrenNodes;
  std::set<ZSwc::SwcTreeNode> m_selectedNodes;
  std::set<ZSwc::SwcTreeNode>::iterator m_extentedSelectionAnchor;
  std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> m_decompsedNodePairs;
  std::vector<ZSwc::SwcTreeNode> m_decomposedNodes;
  std::set<const ZSwc::SwcTreeNode*> m_allNodesSet; // for fast search
  std::set<int> m_allNodeType; // all node type of current swc, used for adjust widget (hide irrelavant stuff)
};

class ZSwcEditCommand : public QUndoCommand
{
public:
  enum class Kind
  {
    Generic = 0,
    MoveSelectedNodes = 1,
  };

  explicit ZSwcEditCommand(const QString& text, ZSwcPack& sp, ZSwc& swcBeforeChange)
    : ZSwcEditCommand(text, Kind::Generic, {}, sp, swcBeforeChange)
  {}

  ZSwcEditCommand(const QString& text,
                  Kind kind,
                  std::vector<int64_t> selectionIds,
                  ZSwcPack& sp,
                  ZSwc& swcBeforeChange)
    : QUndoCommand(text)
    , m_swcPack(sp)
    , m_kind(kind)
    , m_selectionIds(std::move(selectionIds))
  {
    m_swcBeforeChange.swap(swcBeforeChange);
  }

  void undo() override
  {
    m_swcPack.m_swc.swap(m_swcBeforeChange);
    m_swcPack.updateViewRelatedData();
    Q_EMIT m_swcPack.swcChanged();
  }

  void redo() override
  {
    if (!m_firstTimeRedo) {
      m_swcPack.m_swc.swap(m_swcBeforeChange);
    } else {
      m_firstTimeRedo = false;
    }
    m_swcPack.updateViewRelatedData();
    Q_EMIT m_swcPack.swcChanged();
  }

  int id() const override
  {
    if (m_kind == Kind::MoveSelectedNodes) {
      return 1;
    }
    return -1;
  }

  bool mergeWith(const QUndoCommand* other) override
  {
    if (other == nullptr) {
      return false;
    }
    if (other->id() != id() || id() < 0) {
      return false;
    }
    const auto* oth = dynamic_cast<const ZSwcEditCommand*>(other);
    if (oth == nullptr) {
      return false;
    }
    if (m_kind != oth->m_kind) {
      return false;
    }
    if (m_selectionIds != oth->m_selectionIds) {
      return false;
    }
    return true;
  }

protected:
  ZSwcPack& m_swcPack;
  ZSwc m_swcBeforeChange;
  bool m_firstTimeRedo = true;
  Kind m_kind = Kind::Generic;
  std::vector<int64_t> m_selectionIds;
};

} // namespace nim
