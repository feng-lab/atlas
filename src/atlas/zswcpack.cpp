#include "zswcpack.h"

#include "zswcdoc.h"
#include "zminimumspanningtree.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>

namespace nim {

ZSwcPack::ZSwcPack(ZSwc swc, const QString& path, size_t id, ZSwcDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_swc(std::move(swc))
  , m_path(QFileInfo(path).canonicalFilePath())
  , m_doc(doc)
{
  updateDerivedData();
  updateViewRelatedData();
  createContextMenu();
  connect(&m_undoStack, &QUndoStack::cleanChanged,
          this, &ZSwcPack::undoStackCleanChanged);
}

ZSwcPack::~ZSwcPack()
{
  m_undoStack.disconnect(this);
}

const QString& ZSwcPack::info() const
{
  if (m_info.isEmpty()) {
    m_info = QString("size %1").arg(m_swc.size());
  }
  return m_info;
}

QMenu& ZSwcPack::contextMenu()
{
  m_selectCurrentBranchAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_selectBranchUpstreamAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_selectBranchDownstreamAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_selectUpstreamAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_selectDownstreamAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_selectEntireTreeAction->setEnabled(m_extentedSelectionAnchor != m_selectedNodes.end());
  m_deleteSelectedNodesAction->setEnabled(!m_selectedNodes.empty());
  m_setSelectedNodeAsRootAction->setEnabled(m_selectedNodes.size() == 1);
  m_breakSelectedNodesAction->setEnabled(m_selectedNodes.size() > 1);
  m_connectSelectedNodesAction->setEnabled(m_selectedNodes.size() > 1);
  return m_contextMenu;
}

void ZSwcPack::save(const QString &fileName)
{
  m_swc.resortID();
  m_swc.save(fileName);
  m_path = QFileInfo(fileName).canonicalFilePath();
  m_undoStack.setClean();
  updateDerivedData();
}

ZBBox<glm::ivec4> ZSwcPack::boundBox() const
{
  ZBBox<glm::ivec4> res;
  for (auto& p : m_swc) {
    res.expand(glm::ivec4(p.x - std::max(0.5, p.radius), p.y - std::max(0.5, p.radius), std::floor(p.z), 0));
    res.expand(glm::ivec4(p.x + std::max(0.5, p.radius), p.y + std::max(0.5, p.radius), std::ceil(p.z), 0));
  }
  return res;
}

void ZSwcPack::setSelectedNodes(const std::set<ZSwc::SwcTreeNode>& sn)
{
  // LOG(INFO) << "here" << sn.size();
  if (m_selectedNodes == sn) {
    return;
  }
  m_selectedNodes = sn;
  m_extentedSelectionAnchor = m_selectedNodes.empty() ? m_selectedNodes.end() : m_selectedNodes.begin();
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = m_selectedNodes.find(it) != m_selectedNodes.end();
  }
  emit selectionChanged();
}

std::tuple<int, int> ZSwcPack::getParentRowAndRowOfNode(const ZSwc::SwcTreeNode& node) const
{
  CHECK(!ZSwc::isNull(node));
  if (ZSwc::isRoot(node)) {
    for (size_t r = 0; r < m_rootNodes.size(); ++r) {
      if (node == m_rootNodes[r]) {
        return std::make_tuple(static_cast<int>(r), -1);
      }
    }
    CHECK(false);
  } else {
    auto pit = ZSwc::parent(node);
    while (!ZSwc::isRoot(pit)) {
      pit = ZSwc::parent(pit);
    }
    auto it = std::find(m_rootNodes.begin(), m_rootNodes.end(), pit);
    CHECK(it != m_rootNodes.end());
    int parentRow = static_cast<int>(std::distance(m_rootNodes.begin(), it));
    auto& children = m_rootToChildrenNodes.at(m_rootNodes[parentRow]);
    it = std::find(children.begin(), children.end(), node);
    CHECK(it != children.end());
    int row = static_cast<int>(std::distance(children.begin(), it));
    return std::make_tuple(parentRow, row);
  }
}

ZSwc::SwcTreeNode ZSwcPack::getNodeOfParentRowAndRow(const std::tuple<int, int>& prar) const
{
  auto [parentRow, row] = prar;
  CHECK(parentRow >= 0) << parentRow;
  if (row < 0) {
    return m_rootNodes[parentRow];
  } else {
    return m_rootToChildrenNodes.at(m_rootNodes[parentRow])[row];
  }
}

void ZSwcPack::onTreeNodeSelected(const ZSwc::SwcTreeNode* p, bool append, bool extend)
{
  CHECK(!(append && extend));
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    extend = false;
  }
  if (extend) {
    CHECK(m_extentedSelectionAnchor != m_selectedNodes.end());
    if (!p || *p == *m_extentedSelectionAnchor) {
      return;
    }
    // select everything between anchor and p
    auto root = m_swc.lowestCommonAncestor(*m_extentedSelectionAnchor, *p);
    if (ZSwc::isNull(root)) {
      return;
    }
    bool hasChange = m_selectedNodes.find(root) == m_selectedNodes.end();
    if (hasChange) {
      root->selected = true;
      m_selectedNodes.insert(root);
    }
    for (auto it = m_swc.beginAncestor(*p); it != root; ++it) {
      it->selected = true;
      const auto& [ignore, ok] = m_selectedNodes.insert(it);
      hasChange = hasChange || ok;
    }
    for (auto it = m_swc.beginAncestor(*m_extentedSelectionAnchor); it != root; ++it) {
      it->selected = true;
      const auto& [ignore, ok] = m_selectedNodes.insert(it);
      hasChange = hasChange || ok;
    }
    if (hasChange) {
      emit selectionChanged();
    }
  } else if (append) {
    if (!p) {
      return;
    }
    auto pit = m_selectedNodes.find(*p);
    if (pit == m_selectedNodes.end()) {
      const_cast<ZSwc::SwcTreeNode*>(p)->node->data.selected = true;
      auto ip = m_selectedNodes.insert(*p);
      m_extentedSelectionAnchor = ip.first;
      emit selectionChanged();
    } else {
      const_cast<ZSwc::SwcTreeNode*>(p)->node->data.selected = false;
      m_selectedNodes.erase(pit);
      m_extentedSelectionAnchor = m_selectedNodes.end();
      emit selectionChanged();
    }
  } else {
    if (!p && m_selectedNodes.empty()) {
      return;
    }
    if (p && (m_selectedNodes.size() == 1) && (m_selectedNodes.find(*p) != m_selectedNodes.end())) {
      return;
    }
    if (p) {
      for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
        it->selected = it == *p;
      }
      m_selectedNodes = std::set<ZSwc::SwcTreeNode>{*p};
      m_extentedSelectionAnchor = m_selectedNodes.begin();
    } else {
      for (auto& tn : m_swc) {
        tn.selected = false;
      }
      m_selectedNodes.clear();
      m_extentedSelectionAnchor = m_selectedNodes.end();
    }
    emit selectionChanged();
  }
}

void ZSwcPack::updateDerivedData()
{
  m_info.clear();
  m_name = QFileInfo(m_path).fileName();
  m_tooltip = m_path;
}

void ZSwcPack::updateViewRelatedData()
{
  m_rootNodes.clear();
  m_rootToChildrenNodes.clear();
  m_selectedNodes.clear();
  m_allNodeType.clear();
  m_decompsedNodePairs.clear();
  m_decomposedNodes.clear();
  m_allNodesSet.clear();

  for (auto rit = m_swc.beginRoot(); rit != m_swc.endRoot(); ++rit) {
    m_rootNodes.emplace_back(rit);

    m_rootToChildrenNodes[rit].clear();
    auto it = m_swc.begin(rit);
    CHECK(it == rit);
    m_allNodeType.insert(it->type);
    m_decomposedNodes.push_back(it);
    if (it->selected) {
      m_selectedNodes.insert(it);
    }
    for (++it; it != m_swc.end(rit); ++it) {
      m_rootToChildrenNodes[rit].push_back(it);
      m_allNodeType.insert(it->type);
      m_decomposedNodes.push_back(it);
      if (it->selected) {
        m_selectedNodes.insert(it);
      }
      m_decompsedNodePairs.emplace_back(it, ZSwc::parent(it));
    }
  }
  // LOG(INFO) << m_selectedNodes.size() << " selected";
  for (auto& decomposedNode : m_decomposedNodes) {
    m_allNodesSet.insert(&decomposedNode);
  }
  m_extentedSelectionAnchor = m_selectedNodes.empty() ? m_selectedNodes.end() : m_selectedNodes.begin();
}

void ZSwcPack::createContextMenu()
{
  m_selectCurrentBranchAction = new QAction(tr("Select Current Branch"), this);
  m_selectCurrentBranchAction->setStatusTip(tr("Select current branch"));
  connect(m_selectCurrentBranchAction, &QAction::triggered, this, &ZSwcPack::selectCurrentBranch);

  m_selectBranchUpstreamAction = new QAction(tr("Select Branch Upstream"), this);
  m_selectBranchUpstreamAction->setStatusTip(tr("Select upstream till branch start"));
  connect(m_selectBranchUpstreamAction, &QAction::triggered, this, &ZSwcPack::selectBranchUpstream);

  m_selectBranchDownstreamAction = new QAction(tr("Select Branch Downstream"), this);
  m_selectBranchDownstreamAction->setStatusTip(tr("Select downstream till branch end"));
  connect(m_selectBranchDownstreamAction, &QAction::triggered, this, &ZSwcPack::selectBranchDownstream);

  m_selectUpstreamAction = new QAction(tr("Select Upstream"), this);
  m_selectUpstreamAction->setStatusTip(tr("Select upstream till root"));
  connect(m_selectUpstreamAction, &QAction::triggered, this, &ZSwcPack::selectUpstream);

  m_selectDownstreamAction = new QAction(tr("Select Downstream (subtree)"), this);
  m_selectDownstreamAction->setStatusTip(tr("Select subtree"));
  connect(m_selectDownstreamAction, &QAction::triggered, this, &ZSwcPack::selectSubtree);

  m_selectEntireTreeAction = new QAction(tr("Select Entire Tree"), this);
  m_selectEntireTreeAction->setStatusTip(tr("Select the entire tree"));
  connect(m_selectEntireTreeAction, &QAction::triggered, this, &ZSwcPack::selectEntireTree);

  m_deleteSelectedNodesAction = new QAction(tr("Delete Selected Nodes"), this);
  m_deleteSelectedNodesAction->setStatusTip(tr("Delete all selected nodes"));
  connect(m_deleteSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::deleteSelectedNodes);

  m_setSelectedNodeAsRootAction = new QAction(tr("Set Selected Node As Root"), this);
  m_setSelectedNodeAsRootAction->setStatusTip(tr("Set selected node as the root of tree"));
  connect(m_setSelectedNodeAsRootAction, &QAction::triggered, this, &ZSwcPack::setSelectedNodeAsRoot);

  m_breakSelectedNodesAction = new QAction(tr("Break Selected Nodes"), this);
  m_breakSelectedNodesAction->setStatusTip(tr("Break the connections between selected nodes"));
  connect(m_breakSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::breakSelectedNodes);

  m_connectSelectedNodesAction = new QAction(tr("Connect Selected Nodes"), this);
  m_connectSelectedNodesAction->setStatusTip(tr("Connect the selected nodes so they are in one tree"));
  connect(m_connectSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::connectSelectedNodes);

  auto selectMenu = m_contextMenu.addMenu("Select");
  selectMenu->addAction(m_selectCurrentBranchAction);
  selectMenu->addAction(m_selectBranchUpstreamAction);
  selectMenu->addAction(m_selectBranchDownstreamAction);
  selectMenu->addAction(m_selectUpstreamAction);
  selectMenu->addAction(m_selectDownstreamAction);
  selectMenu->addAction(m_selectEntireTreeAction);
  m_contextMenu.addAction(m_deleteSelectedNodesAction);
  m_contextMenu.addAction(m_setSelectedNodeAsRootAction);
  m_contextMenu.addAction(m_breakSelectedNodesAction);
  m_contextMenu.addAction(m_connectSelectedNodesAction);
}

void ZSwcPack::selectCurrentBranch()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = *m_extentedSelectionAnchor;
  if (ZSwc::isRoot(startNode)) {
    return;
  }
  bool hasChange = false;
  for (auto it = m_swc.beginAncestor(startNode); it != m_swc.endAncestor(startNode);) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
    ++it;
    if (ZSwc::isBranchNode(it) || ZSwc::isRoot(it)) {
      break;
    }
  }
  if (!ZSwc::isBranchNode(startNode)) {
    for (auto it = m_swc.begin(startNode); it != m_swc.end(startNode); ++it) {
      it->selected = true;
      const auto&[ignore, ok] = m_selectedNodes.insert(it);
      hasChange = hasChange || ok;
      if (ZSwc::isBranchNode(it) || ZSwc::isRoot(it)) {
        break;
      }
    }
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::selectBranchUpstream()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = *m_extentedSelectionAnchor;
  if (ZSwc::isRoot(startNode)) {
    return;
  }
  bool hasChange = false;
  for (auto it = m_swc.beginAncestor(startNode); it != m_swc.endAncestor(startNode);) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
    ++it;
    if (ZSwc::isBranchNode(it) || ZSwc::isRoot(it)) {
      break;
    }
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::selectBranchDownstream()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = *m_extentedSelectionAnchor;
  if (ZSwc::isRoot(startNode)) {
    return;
  }
  bool hasChange = false;
  for (auto it = m_swc.begin(startNode); it != m_swc.end(startNode); ++it) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
    if (ZSwc::isBranchNode(it) || ZSwc::isRoot(it)) {
      break;
    }
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::selectUpstream()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = *m_extentedSelectionAnchor;
  bool hasChange = false;
  for (auto it = m_swc.beginAncestor(startNode); it != m_swc.endAncestor(startNode); ++it) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::selectSubtree()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = *m_extentedSelectionAnchor;
  bool hasChange = false;
  for (auto it = m_swc.begin(startNode); it != m_swc.end(startNode); ++it) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::selectEntireTree()
{
  if (m_extentedSelectionAnchor == m_selectedNodes.end()) {
    return;
  }
  auto startNode = ZSwc::root(*m_extentedSelectionAnchor);
  bool hasChange = false;
  for (auto it = m_swc.begin(startNode); it != m_swc.end(startNode); ++it) {
    it->selected = true;
    const auto&[ignore, ok] = m_selectedNodes.insert(it);
    hasChange = hasChange || ok;
  }
  if (hasChange) {
    emit selectionChanged();
  }
}

void ZSwcPack::deleteSelectedNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }
  ZSwc swcBeforeChange = m_swc;
  for (auto p : m_selectedNodes) {
    m_swc.erase(p);
  }
  m_undoStack.push(new ZSwcEditCommand(QString("Deleted Selected %1 Nodes").arg(m_selectedNodes.size()),
                                       *this, swcBeforeChange));
}

void ZSwcPack::setSelectedNodeAsRoot()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }
  ZSwc swcBeforeChange = m_swc;
  m_swc.setAsRoot(*m_selectedNodes.begin());
  m_undoStack.push(new ZSwcEditCommand(QString("Set Selected Node As Root"),
                                       *this, swcBeforeChange));
}

void ZSwcPack::breakSelectedNodes()
{
  if (m_selectedNodes.size() < 2) {
    return;
  }
  std::vector<const ZSwc::SwcTreeNode*> selectedNodeVector;
  for (auto& p : m_selectedNodes) {
    selectedNodeVector.push_back(&p);
  }
  bool hasChange = false;
  ZSwc swcBeforeChange = m_swc;
  for (size_t i = 1; i < selectedNodeVector.size(); ++i) {
    auto node1 = *selectedNodeVector[i - 1];
    auto node2 = *selectedNodeVector[i];
    if (!ZSwc::inSameTree(node1, node2)) {
      continue;
    }
    auto canode = m_swc.lowestCommonAncestor(node1, node2);
    if (canode == node1) {
      m_swc.appendRoot(node2);
    } else if (canode == node2) {
      m_swc.appendRoot(node1);
    } else {
      m_swc.appendRoot(node1->radius > node2->radius ? node1 : node2);
    }
    hasChange = true;
  }
  if (!hasChange) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QString("Break Selected %1 Nodes").arg(m_selectedNodes.size()),
                                       *this, swcBeforeChange));
}

void ZSwcPack::connectSelectedNodes()
{
  if (m_selectedNodes.size() < 2) {
    return;
  }
  std::vector<const ZSwc::SwcTreeNode*> selectedNodeVector;
  for (auto& p : m_selectedNodes) {
    selectedNodeVector.push_back(&p);
  }
  bool hasChange = false;
  ZMinimumSpanningTree mst;
  for (size_t i = 0; i < selectedNodeVector.size(); ++i) {
    for (size_t j = i + 1; j < selectedNodeVector.size(); ++j) {
      auto node1 = *selectedNodeVector[i];
      auto node2 = *selectedNodeVector[j];
      double weight = -1e4;
      if (!ZSwc::inSameTree(node1, node2)) {
        hasChange = true;
        weight = glm::length(glm::dvec3(node1->x, node1->y, node1->z) -
                             glm::dvec3(node2->x, node2->y, node2->z)) -
                               node1->radius -
                               node2->radius;
      }
      mst.addEdge(i, j, weight);
    }
  }
  if (!hasChange) {
    return;
  }
  ZSwc swcBeforeChange = m_swc;

  for (auto&& [i1, i2] : mst.runMST()) {
    // LOG(INFO) << i1 << " " << i2;
    auto node1 = *selectedNodeVector[i1];
    auto node2 = *selectedNodeVector[i2];
    if (ZSwc::inSameTree(node1, node2)) {
      continue;
    }
    m_swc.setAsRoot(node1);
    m_swc.setAsRoot(node2);
    if (node1->radius > node2->radius) {
      m_swc.appendChild(node1, node2);
    } else {
      m_swc.appendChild(node2, node1);
    }
  }
  m_undoStack.push(new ZSwcEditCommand(QString("Connect Selected %1 Nodes").arg(m_selectedNodes.size()),
                                       *this, swcBeforeChange));
}

} // namespace nim


