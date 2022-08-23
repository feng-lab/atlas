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

  inline const QString& name() const
  {
    return m_name;
  }

  inline const QString& tooltip() const
  {
    return m_tooltip;
  }

  inline const QString& path() const
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

  inline const ZSwc& swc() const
  {
    return m_swc;
  }

  ZBBox<glm::ivec4> boundBox() const;

  inline const std::vector<ZSwc::SwcTreeNode>& rootNodes() const
  {
    return m_rootNodes;
  }

  inline const std::map<ZSwc::SwcTreeNode, std::vector<ZSwc::SwcTreeNode>>& rootToChildrenNodes() const
  {
    return m_rootToChildrenNodes;
  }

  inline const std::set<ZSwc::SwcTreeNode>& selectedNodes() const
  {
    return m_selectedNodes;
  }

  inline const std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>>& decompsedNodePairs() const
  {
    return m_decompsedNodePairs;
  }

  inline const std::vector<ZSwc::SwcTreeNode>& decomposedNodes() const
  {
    return m_decomposedNodes;
  }

  inline const std::set<const ZSwc::SwcTreeNode*>& allNodesSet() const
  {
    return m_allNodesSet;
  }

  inline const std::set<int>& allNodeType() const
  {
    return m_allNodeType;
  }

  void setSelectedNodes(const std::set<ZSwc::SwcTreeNode>& sn);

  std::tuple<int, int> getParentRowAndRowOfNode(const ZSwc::SwcTreeNode& node) const;

  ZSwc::SwcTreeNode getNodeOfParentRowAndRow(const std::tuple<int, int>& prar) const;

  void onTreeNodeSelected(const ZSwc::SwcTreeNode* p, bool append, bool extend);

  void deleteSelectedNodes();

protected:
  void updateViewRelatedData();

  void createContextMenu();

  void selectCurrentBranch();

  void selectBranchUpstream();

  void selectBranchDownstream();

  void selectUpstream();

  void selectSubtree();

  void selectEntireTree();

  void setSelectedNodeAsRoot();

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

  QAction* m_selectCurrentBranchAction = nullptr;
  QAction* m_selectBranchUpstreamAction = nullptr;
  QAction* m_selectBranchDownstreamAction = nullptr;
  QAction* m_selectUpstreamAction = nullptr;
  QAction* m_selectDownstreamAction = nullptr;
  QAction* m_selectEntireTreeAction = nullptr;
  QAction* m_deleteSelectedNodesAction = nullptr;
  QAction* m_setSelectedNodeAsRootAction = nullptr;
  QAction* m_breakSelectedNodesAction = nullptr;
  QAction* m_connectSelectedNodesAction = nullptr;
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
  explicit ZSwcEditCommand(const QString& text, ZSwcPack& sp, ZSwc& swcBeforeChange)
    : QUndoCommand(text)
    , m_swcPack(sp)
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

protected:
  ZSwcPack& m_swcPack;
  ZSwc m_swcBeforeChange;
  bool m_firstTimeRedo = true;
};

} // namespace nim
