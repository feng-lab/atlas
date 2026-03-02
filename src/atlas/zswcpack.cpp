#include "zswcpack.h"

#include "zswcdoc.h"
#include "zimgdoc.h"
#include "zimgpack.h"
#include "zimgpackvoxelvolume.h"
#include "zsysteminfo.h"
#include "ztracesettings.h"
#include "zinformationdialog.h"
#include "zresolutiondialog.h"
#include "zswcskeletontransformdialog.h"
#include "zswcsizedialog.h"
#include "zminimumspanningtree.h"
#include "zarrayfilters.h"
#include "zswceditlegacy.h"
#include "zswctreenodegeomlegacy.h"
#include "zswcresampler.h"
#include "zneutubetracep2p.h"
#include "zneutubetraceconfig.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QApplication>
#include <QKeySequence>
#include <QMessageBox>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <limits>

namespace nim {

namespace {

[[nodiscard]] bool hasChild(const ZSwc::SwcTreeNode& tn)
{
  return !ZSwc::isNull(tn) && !ZSwc::isLeaf(tn);
}

[[nodiscard]] double dist3D(const SwcNode& a, const SwcNode& b)
{
  return glm::length(glm::dvec3(a.x, a.y, a.z) - glm::dvec3(b.x, b.y, b.z));
}

[[nodiscard]] double dist2D(const SwcNode& a, const SwcNode& b)
{
  return glm::length(glm::dvec2(a.x, a.y) - glm::dvec2(b.x, b.y));
}

} // namespace

ZSwcPack::ZSwcPack(ZSwc swc, const QString& path, size_t id, ZSwcDoc& doc, QObject* parent)
  : ZObjPack(id, &doc, parent)
  , m_swc(std::move(swc))
  , m_path()
  , m_doc(doc)
{
  if (!path.isEmpty()) {
    const QFileInfo fi(path);
    m_path = fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath();
  }
  updateDerivedData();
  updateViewRelatedData();
  createContextMenu();
  connect(&m_undoStack, &QUndoStack::cleanChanged, this, &ZSwcPack::undoStackCleanChanged);
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

ZSwc::SwcTreeNode ZSwcPack::findNodeByIdOrNull(int64_t id)
{
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    if (it->id == id) {
      return it;
    }
  }
  return {};
}

QMenu& ZSwcPack::contextMenu()
{
  const bool haveSelection = !m_selectedNodes.empty();
  m_selectDownstreamAction->setEnabled(haveSelection);
  m_selectUpstreamAction->setEnabled(haveSelection);
  m_selectNeighborAction->setEnabled(haveSelection);
  m_selectHostBranchAction->setEnabled(haveSelection);
  m_selectConnectedAction->setEnabled(haveSelection);
  m_selectAllAction->setEnabled(!m_swc.empty());
  m_deleteSelectedNodesAction->setEnabled(!m_selectedNodes.empty());
  if (m_deleteUnselectedNodesAction != nullptr) {
    m_deleteUnselectedNodesAction->setEnabled(!m_selectedNodes.empty() && m_selectedNodes.size() < m_swc.size());
  }
  m_setSelectedNodeAsRootAction->setEnabled(m_selectedNodes.size() == 1);
  if (m_changeSelectedNodeTypeAction != nullptr) {
    m_changeSelectedNodeTypeAction->setEnabled(haveSelection);
  }
  if (m_translateSelectedNodesAction != nullptr) {
    m_translateSelectedNodesAction->setEnabled(haveSelection);
  }
  if (m_changeSelectedNodeSizeAction != nullptr) {
    m_changeSelectedNodeSizeAction->setEnabled(haveSelection);
  }
  m_breakSelectedNodesAction->setEnabled(m_selectedNodes.size() > 1);
  m_connectSelectedNodesAction->setEnabled(m_selectedNodes.size() > 1);
  if (m_mergeSelectedNodesAction != nullptr) {
    m_mergeSelectedNodesAction->setEnabled(m_selectedNodes.size() > 1);
  }
  if (m_insertNodeAction != nullptr) {
    m_insertNodeAction->setEnabled(m_selectedNodes.size() > 1);
  }
  if (m_interpolateAction != nullptr) {
    m_interpolateAction->setEnabled(!m_selectedNodes.empty());
  }
  if (m_interpolateZAction != nullptr) {
    m_interpolateZAction->setEnabled(!m_selectedNodes.empty());
  }
  if (m_interpolatePositionAction != nullptr) {
    m_interpolatePositionAction->setEnabled(!m_selectedNodes.empty());
  }
  if (m_interpolateRadiusAction != nullptr) {
    m_interpolateRadiusAction->setEnabled(!m_selectedNodes.empty());
  }
  if (m_removeTurnAction != nullptr) {
    m_removeTurnAction->setEnabled(m_selectedNodes.size() == 1);
  }
  if (m_resolveCrossoverAction != nullptr) {
    m_resolveCrossoverAction->setEnabled(m_selectedNodes.size() == 1);
  }
  if (m_joinIsolatedBranchAction != nullptr) {
    m_joinIsolatedBranchAction->setEnabled(m_selectedNodes.size() == 1);
  }
  if (m_joinIsolatedBranchAcrossTreesAction != nullptr) {
    m_joinIsolatedBranchAcrossTreesAction->setEnabled(m_selectedNodes.size() == 1);
  }
  if (m_resetBranchPointAction != nullptr) {
    m_resetBranchPointAction->setEnabled(m_selectedNodes.size() == 1);
  }
  if (m_showSelectedBranchLengthAction != nullptr) {
    m_showSelectedBranchLengthAction->setEnabled(!m_selectedNodes.empty());
  }
  if (m_showSelectedBranchScaledLengthAction != nullptr) {
    m_showSelectedBranchScaledLengthAction->setEnabled(!m_selectedNodes.empty());
  }
  return m_contextMenu;
}

void ZSwcPack::save(const QString& fileName)
{
  m_swc.resortID();
  m_swc.save(fileName);
  m_path = QFileInfo(fileName).canonicalFilePath();
  m_undoStack.setClean();
  updateDerivedData();
}

void ZSwcPack::replaceSwcWithUndo(const QString& undoText, ZSwc newSwc)
{
  ZSwc swcBeforeChange = m_swc;
  m_swc = std::move(newSwc);
  m_undoStack.push(new ZSwcEditCommand(undoText, *this, swcBeforeChange));
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
  // VLOG(1) << "here" << sn.size();
  if (m_selectedNodes == sn) {
    return;
  }
  m_selectedNodes = sn;
  m_extentedSelectionAnchor = m_selectedNodes.empty() ? m_selectedNodes.end() : m_selectedNodes.begin();
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = m_selectedNodes.contains(it);
  }
  Q_EMIT selectionChanged();
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
    auto parentRow = static_cast<int>(indexOf(m_rootNodes, pit));
    CHECK(parentRow >= 0);
    auto& allChildren = m_rootToChildrenNodes.at(m_rootNodes[parentRow]);
    auto row = static_cast<int>(indexOf(allChildren, node));
    CHECK(row >= 0);
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
    bool hasChange = !m_selectedNodes.contains(root);
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
      Q_EMIT selectionChanged();
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
      Q_EMIT selectionChanged();
    } else {
      const_cast<ZSwc::SwcTreeNode*>(p)->node->data.selected = false;
      m_selectedNodes.erase(pit);
      m_extentedSelectionAnchor = m_selectedNodes.end();
      Q_EMIT selectionChanged();
    }
  } else {
    if (!p && m_selectedNodes.empty()) {
      return;
    }
    if (p && (m_selectedNodes.size() == 1) && m_selectedNodes.contains(*p)) {
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
    Q_EMIT selectionChanged();
  }
}

void ZSwcPack::extendSelectedNodePlain(const glm::dvec3& center, double radius)
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const ZSwc::SwcTreeNode parentNode = *m_selectedNodes.begin();
  CHECK(!ZSwc::isNull(parentNode));

  SwcNode child;
  child.id = 0;
  child.type = 0;
  child.x = center.x;
  child.y = center.y;
  child.z = center.z;
  child.radius = radius;

  ZSwc swcBeforeChange = m_swc;
  const ZSwc::SwcTreeNode newNode = m_swc.appendChild(parentNode, child);

  // neuTube parity: continuous extending selects the newly created node only.
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = (it == newNode);
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Extend SWC node"), *this, swcBeforeChange));
}

namespace {

[[nodiscard]] std::vector<ZSwc::SwcTreeNode> buildFirstChildChain(const ZSwc::SwcTreeNode& root)
{
  std::vector<ZSwc::SwcTreeNode> chain;
  if (ZSwc::isNull(root)) {
    return chain;
  }

  chain.reserve(128);
  ZSwc::SwcTreeNode cur = root;
  chain.push_back(cur);
  while (hasChild(cur)) {
    cur = ZSwc::firstChild(cur);
    chain.push_back(cur);
  }
  return chain;
}

void smoothChainRadiusLegacyLike(const std::vector<ZSwc::SwcTreeNode>& chain, bool excludingBranchPoint)
{
  if (chain.empty()) {
    return;
  }

  std::vector<double> valueArray(chain.size());
  for (size_t i = 0; i < chain.size(); ++i) {
    valueArray[i] = chain[i]->radius;
  }

  std::vector<double> tmp(chain.size());
  medianFilter1DLegacyLike(valueArray.data(), valueArray.size(), /*wndsize*/ 5, tmp.data());
  averageSmooth1DLegacyLike(tmp.data(), tmp.size(), /*wndsize*/ 3, valueArray.data());

  for (size_t i = 0; i < chain.size(); ++i) {
    if (excludingBranchPoint && ZSwc::isBranchNode(chain[i])) {
      continue;
    }
    chain[i]->radius = valueArray[i];
  }
}

void smoothChainZLegacyLike(const std::vector<ZSwc::SwcTreeNode>& chain)
{
  if (chain.empty()) {
    return;
  }

  std::vector<double> valueArray(chain.size());
  for (size_t i = 0; i < chain.size(); ++i) {
    valueArray[i] = chain[i]->z;
  }

  std::vector<double> tmp(chain.size());
  medianFilter1DLegacyLike(valueArray.data(), valueArray.size(), /*wndsize*/ 5, tmp.data());
  averageSmooth1DLegacyLike(tmp.data(), tmp.size(), /*wndsize*/ 3, valueArray.data());

  for (size_t i = 0; i < chain.size(); ++i) {
    chain[i]->z = valueArray[i];
  }
}

[[nodiscard]] bool mergeLeafToParentAverageLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& leaf)
{
  if (ZSwc::isNull(leaf)) {
    return false;
  }
  const ZSwc::SwcTreeNode parent = ZSwc::parent(leaf);
  if (ZSwc::isNull(parent)) {
    return false;
  }

  swcNodeAverageLegacyLike(ZSwc::ConstSwcTreeNode(parent), ZSwc::ConstSwcTreeNode(leaf), parent);
  swc.erase(leaf);
  return true;
}

} // namespace

void ZSwcPack::extendSelectedNodeSmartLegacyLike(const glm::dvec3& center, double radius, size_t t)
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const ZSwc::SwcTreeNode prevNode = *m_selectedNodes.begin();
  CHECK(!ZSwc::isNull(prevNode));

  if (!(center.x >= 0.0 && center.y >= 0.0 && center.z >= 0.0)) {
    return;
  }

  ZDoc& doc = m_doc.doc();
  const ZTraceSettings& settings = doc.traceSettings();
  const std::optional<size_t> imgIdOpt = settings.sourceImageId();
  if (!imgIdOpt.has_value()) {
    LOG(WARNING) << "SWC smart-extend: no source image selected in Trace Settings.";
    return;
  }

  const size_t sc = settings.sourceChannel();

  ZImgDoc& imgDoc = doc.imgDoc();
  if (!imgDoc.hasObjWithID(*imgIdOpt)) {
    LOG(WARNING) << "SWC smart-extend: source image id " << *imgIdOpt << " is not loaded.";
    return;
  }

  const std::shared_ptr<ZImgPack> imgPack = imgDoc.imgPackShared(*imgIdOpt);
  if (!imgPack) {
    LOG(WARNING) << "SWC smart-extend: source image pack is not available.";
    return;
  }

  const ZImgInfo info = imgPack->imgInfo();
  if (sc >= info.numChannels || t >= info.numTimes) {
    LOG(WARNING) << "SWC smart-extend: invalid channel/time selection (c=" << sc << ", t=" << t << ") for image <"
                 << info << ">.";
    return;
  }

  TraceConfig cfg;
  const QString traceCfgPath = QDir(ZSystemInfo::jsonDirPath()).absoluteFilePath(QStringLiteral("trace_config.json"));
  const bool ok = loadTraceConfigLegacyLike(traceCfgPath.toStdString(), cfg);
  if (!ok) {
    cfg = TraceConfig{};
  }

  if (settings.algoConfigInitialized()) {
    const ZTraceSettings::AlgoConfig algoCfg = settings.algoConfig();
    cfg.minAutoScore = algoCfg.minAutoScore;
    cfg.minManualScore = algoCfg.minManualScore;
    cfg.minSeedScore = algoCfg.minSeedScore;
    cfg.min2dScore = algoCfg.min2dScore;
    cfg.refit = algoCfg.refit;
    cfg.spTest = algoCfg.spTest;
    cfg.crossoverTest = algoCfg.crossoverTest;
    cfg.tuneEnd = algoCfg.tuneEnd;
    cfg.edgePath = algoCfg.edgePath;
    cfg.enhanceMask = algoCfg.enhanceMask;
    cfg.seedMethod = algoCfg.seedMethod;
    cfg.recover = algoCfg.recover;
    cfg.chainScreenCount = algoCfg.chainScreenCount;
    cfg.maxEucDist = algoCfg.maxEucDist;
  }

  const std::array<double, 3> start{prevNode->x, prevNode->y, prevNode->z};
  const double startRadius = prevNode->radius;
  const std::array<double, 3> target{center.x, center.y, center.z};
  const double targetRadius = radius;

  std::unique_ptr<ZSwc> branch;
  if (imgPack->isDiskCached()) {
    const ZImgPackVoxelVolume signalVol(imgPack, sc, t);
    branch = tracePointToPointLegacyLike(signalVol,
                                         start,
                                         startRadius,
                                         target,
                                         targetRadius,
                                         cfg,
                                         ZNeutubeImageBackgroundLegacyLike::Dark);
  } else {
    const ZImg& signal = imgPack->img();
    if (signal.isEmpty()) {
      LOG(WARNING) << "SWC smart-extend: the source image is empty.";
      return;
    }
    branch = tracePointToPointLegacyLike(signal,
                                         sc,
                                         t,
                                         start,
                                         startRadius,
                                         target,
                                         targetRadius,
                                         cfg,
                                         ZNeutubeImageBackgroundLegacyLike::Dark);
  }
  if (!branch || branch->empty()) {
    return;
  }

  auto rootIt = branch->beginRoot();
  if (rootIt == branch->endRoot()) {
    return;
  }
  const ZSwc::SwcTreeNode root = rootIt;

  const ZSwc::SwcTreeNode branchBegin0 = ZSwc::firstChild(root);
  if (ZSwc::isNull(branchBegin0)) {
    return;
  }

  const std::vector<ZSwc::SwcTreeNode> rootToLeafChain = buildFirstChildChain(root);
  if (rootToLeafChain.size() < 2) {
    return;
  }

  // neuTube: smoothRadius(true) (excluding branch points) then smoothZ().
  smoothChainRadiusLegacyLike(rootToLeafChain, /*excludingBranchPoint*/ true);
  smoothChainZLegacyLike(rootToLeafChain);

  ZSwc::SwcTreeNode branchBegin = branchBegin0;
  if (hasChild(branchBegin)) {
    if (swcNodesHasSignificantOverlapLegacyLike(ZSwc::ConstSwcTreeNode(branchBegin),
                                                ZSwc::ConstSwcTreeNode(prevNode))) {
      branchBegin = ZSwc::firstChild(branchBegin);
    }
  }
  if (ZSwc::isNull(branchBegin)) {
    return;
  }

  // Copy the computed branch chain into a standalone SWC rooted at `branchBegin`.
  std::vector<SwcNode> chainNodes;
  chainNodes.reserve(128);
  {
    ZSwc::SwcTreeNode cur = branchBegin;
    while (!ZSwc::isNull(cur)) {
      chainNodes.push_back(*cur);
      if (!hasChild(cur)) {
        break;
      }
      cur = ZSwc::firstChild(cur);
    }
  }
  if (chainNodes.empty()) {
    return;
  }

  ZSwc newBranchSwc;
  ZSwc::SwcTreeNode begin = newBranchSwc.appendRoot(chainNodes.front());
  for (size_t i = 1; i < chainNodes.size(); ++i) {
    begin = newBranchSwc.appendChild(begin, chainNodes[i]);
  }

  // neuTube: correctTurn() down the chain (skipping the root).
  {
    auto cur = newBranchSwc.beginRoot();
    if (cur != newBranchSwc.endRoot()) {
      ZSwc::SwcTreeNode tn = cur;
      while (hasChild(tn)) {
        tn = ZSwc::firstChild(tn);
        swcNodeCorrectTurnLegacyLike(newBranchSwc, tn);
      }
    }
  }

  // neuTube: resample with ignoreInterRedundant(true).
  {
    ZNeutubeSwcResampler resampler;
    resampler.ignoreInterRedundant(true);
    resampler.optimalDownsample(newBranchSwc);
  }

  auto beginRoot2 = newBranchSwc.beginRoot();
  if (beginRoot2 == newBranchSwc.endRoot()) {
    return;
  }
  ZSwc::SwcTreeNode begin2 = beginRoot2;
  ZSwc::SwcTreeNode leaf2 = begin2;
  int count = 1;
  while (hasChild(leaf2)) {
    leaf2 = ZSwc::firstChild(leaf2);
    ++count;
  }

  // neuTube: if the resampled chain is only 2 nodes and they overlap, merge them.
  if (count == 2) {
    if (swcNodesHasSignificantOverlapLegacyLike(ZSwc::ConstSwcTreeNode(leaf2), ZSwc::ConstSwcTreeNode(begin2))) {
      if (mergeLeafToParentAverageLegacyLike(newBranchSwc, leaf2)) {
        leaf2 = begin2;
      }
    }
  }

  // Attach the computed chain as a child chain of the selected node and select the final leaf only.
  std::vector<SwcNode> finalNodes;
  finalNodes.reserve(newBranchSwc.size());
  {
    ZSwc::SwcTreeNode cur = begin2;
    while (!ZSwc::isNull(cur)) {
      finalNodes.push_back(*cur);
      if (!hasChild(cur)) {
        break;
      }
      cur = ZSwc::firstChild(cur);
    }
  }
  if (finalNodes.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;

  ZSwc::SwcTreeNode inserted = m_swc.appendChild(prevNode, finalNodes.front());
  for (size_t i = 1; i < finalNodes.size(); ++i) {
    inserted = m_swc.appendChild(inserted, finalNodes[i]);
  }
  const ZSwc::SwcTreeNode insertedLeaf = inserted;

  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = false;
  }
  insertedLeaf->selected = true;

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Extend SWC node (path computation)"), *this, swcBeforeChange));
}

void ZSwcPack::addIsolatedNodeLegacyLike(const glm::dvec3& center, double radius)
{
  if (radius <= 0.0) {
    return;
  }

  SwcNode node;
  node.id = 0;
  node.type = 0;
  node.x = center.x;
  node.y = center.y;
  node.z = center.z;
  node.radius = radius;
  node.parentID = -1;

  ZSwc swcBeforeChange = m_swc;
  m_swc.appendRoot(node);

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Add Neuron Node"), *this, swcBeforeChange));
}

void ZSwcPack::setSelectedNodesZLegacyLike(double z)
{
  if (m_selectedNodes.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  bool anyChange = false;
  for (const auto& n : m_selectedNodes) {
    if (n->z != z) {
      n->z = z;
      anyChange = true;
    }
  }

  if (!anyChange) {
    return;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Change Z of Selected Node"), *this, swcBeforeChange));
}

void ZSwcPack::translateSelectedNodesLegacyLike(double dx, double dy, double dz)
{
  if (m_selectedNodes.empty()) {
    return;
  }
  if (dx == 0.0 && dy == 0.0 && dz == 0.0) {
    return;
  }

  std::vector<int64_t> selectedIds;
  selectedIds.reserve(m_selectedNodes.size());
  for (const auto& n : m_selectedNodes) {
    selectedIds.push_back(n->id);
  }
  std::sort(selectedIds.begin(), selectedIds.end());

  ZSwc swcBeforeChange = m_swc;
  for (const auto& n : m_selectedNodes) {
    n->x += dx;
    n->y += dy;
    n->z += dz;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Move Selected Node"),
                                       ZSwcEditCommand::Kind::MoveSelectedNodes,
                                       std::move(selectedIds),
                                       *this,
                                       swcBeforeChange));
}

bool ZSwcPack::connectSelectedNodeToLegacyLike(const ZSwc::SwcTreeNode& target)
{
  if (m_selectedNodes.size() != 1) {
    return false;
  }
  if (ZSwc::isNull(target)) {
    return false;
  }

  const ZSwc::SwcTreeNode anchor = *m_selectedNodes.begin();
  CHECK(!ZSwc::isNull(anchor));
  if (anchor == target) {
    return false;
  }

  // Legacy guard: do nothing when already connected.
  if (!ZSwc::isNull(m_swc.lowestCommonAncestor(anchor, target))) {
    return false;
  }

  ZSwc swcBeforeChange = m_swc;

  m_swc.setAsRoot(target);
  m_swc.appendChild(anchor, target);

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Connect SWC node"), *this, swcBeforeChange));
  return true;
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
  // VLOG(1) << m_selectedNodes.size() << " selected";
  for (auto& decomposedNode : m_decomposedNodes) {
    m_allNodesSet.insert(&decomposedNode);
  }
  m_extentedSelectionAnchor = m_selectedNodes.empty() ? m_selectedNodes.end() : m_selectedNodes.begin();
}

void ZSwcPack::createContextMenu()
{
  m_deleteSelectedNodesAction = new QAction(tr("Delete"), this);
  m_deleteSelectedNodesAction->setShortcut(Qt::Key_X);
  m_deleteSelectedNodesAction->setStatusTip(tr("Delete all selected nodes"));
  connect(m_deleteSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::deleteSelectedNodes);

  m_deleteUnselectedNodesAction = new QAction(tr("Delete Unselected"), this);
  m_deleteUnselectedNodesAction->setStatusTip(tr("Delete all unselected nodes"));
  connect(m_deleteUnselectedNodesAction, &QAction::triggered, this, &ZSwcPack::deleteUnselectedNodes);

  m_breakSelectedNodesAction = new QAction(tr("Break"), this);
  m_breakSelectedNodesAction->setShortcut(Qt::Key_B);
  m_breakSelectedNodesAction->setStatusTip(tr("Break connections between selected nodes"));
  connect(m_breakSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::breakSelectedNodes);

  m_connectSelectedNodesAction = new QAction(tr("Connect"), this);
  m_connectSelectedNodesAction->setShortcut(Qt::Key_C);
  m_connectSelectedNodesAction->setStatusTip(tr("Connect selected nodes so they are in one tree"));
  connect(m_connectSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::connectSelectedNodes);

  m_mergeSelectedNodesAction = new QAction(tr("Merge"), this);
  m_mergeSelectedNodesAction->setStatusTip(tr("Merge selected nodes into a single node"));
  connect(m_mergeSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::mergeSelectedNodes);

  m_insertNodeAction = new QAction(tr("Insert"), this);
  m_insertNodeAction->setShortcut(Qt::Key_I);
  m_insertNodeAction->setStatusTip(tr("Insert nodes between adjacent selected nodes"));
  connect(m_insertNodeAction, &QAction::triggered, this, &ZSwcPack::insertNodesBetweenSelectedPairs);

  m_interpolateAction = new QAction(tr("Position and Radius"), this);
  m_interpolateAction->setStatusTip(tr("Interpolate position and radius of selected branch nodes"));
  connect(m_interpolateAction, &QAction::triggered, this, &ZSwcPack::interpolateSelectedNodes);

  m_interpolateZAction = new QAction(tr("Z"), this);
  m_interpolateZAction->setStatusTip(tr("Interpolate Z coordinate of selected branch nodes"));
  connect(m_interpolateZAction, &QAction::triggered, this, &ZSwcPack::interpolateSelectedNodesZ);

  m_interpolatePositionAction = new QAction(tr("Position"), this);
  m_interpolatePositionAction->setStatusTip(tr("Interpolate XYZ position of selected branch nodes"));
  connect(m_interpolatePositionAction, &QAction::triggered, this, &ZSwcPack::interpolateSelectedNodesPosition);

  m_interpolateRadiusAction = new QAction(tr("Radius"), this);
  m_interpolateRadiusAction->setStatusTip(tr("Interpolate radius of selected branch nodes"));
  connect(m_interpolateRadiusAction, &QAction::triggered, this, &ZSwcPack::interpolateSelectedNodesRadius);

  m_showSummaryAction = new QAction(tr("Summary"), this);
  m_showSummaryAction->setStatusTip(tr("Show basic SWC statistics"));
  connect(m_showSummaryAction, &QAction::triggered, this, &ZSwcPack::showSwcSummary);

  m_showSelectedBranchLengthAction = new QAction(tr("Path length"), this);
  m_showSelectedBranchLengthAction->setStatusTip(tr("Measure total length of selected branches"));
  connect(m_showSelectedBranchLengthAction, &QAction::triggered, this, &ZSwcPack::showSelectedBranchLength);

  m_selectDownstreamAction = new QAction(tr("Downstream"), this);
  m_selectDownstreamAction->setStatusTip(tr("Select downstream nodes"));
  connect(m_selectDownstreamAction, &QAction::triggered, this, &ZSwcPack::selectDownstreamNodes);

  m_selectUpstreamAction = new QAction(tr("Upstream"), this);
  m_selectUpstreamAction->setStatusTip(tr("Select upstream nodes"));
  connect(m_selectUpstreamAction, &QAction::triggered, this, &ZSwcPack::selectUpstreamNodes);

  m_selectNeighborAction = new QAction(tr("Neighbors"), this);
  m_selectNeighborAction->setStatusTip(
    tr("Select neighbors (directly connected nodes) of the currently selected nodes"));
  connect(m_selectNeighborAction, &QAction::triggered, this, &ZSwcPack::selectNeighborNodes);

  m_selectHostBranchAction = new QAction(tr("Host branch"), this);
  m_selectHostBranchAction->setStatusTip(tr("Select branches containing the currently selected nodes"));
  connect(m_selectHostBranchAction, &QAction::triggered, this, &ZSwcPack::selectHostBranchNodes);

  m_selectConnectedAction = new QAction(tr("All connected nodes"), this);
  m_selectConnectedAction->setStatusTip(
    tr("Select all nodes connected (directly or indirectly) of the currently selected nodes"));
  connect(m_selectConnectedAction, &QAction::triggered, this, &ZSwcPack::selectConnectedNodes);

  m_selectAllAction = new QAction(tr("All nodes"), this);
  m_selectAllAction->setShortcut(QKeySequence::SelectAll);
  m_selectAllAction->setStatusTip(tr("Select all nodes"));
  connect(m_selectAllAction, &QAction::triggered, this, &ZSwcPack::selectAllNodes);

  m_changeSelectedNodeTypeAction = new QAction(tr("Change type"), this);
  m_changeSelectedNodeTypeAction->setStatusTip(tr("Change type of the selected nodes"));
  connect(m_changeSelectedNodeTypeAction, &QAction::triggered, this, &ZSwcPack::changeSelectedNodeType);

  m_translateSelectedNodesAction = new QAction(tr("Translate"), this);
  m_translateSelectedNodesAction->setStatusTip(tr("Translate selected nodes"));
  connect(m_translateSelectedNodesAction, &QAction::triggered, this, &ZSwcPack::translateSelectedNodes);

  m_changeSelectedNodeSizeAction = new QAction(tr("Change size"), this);
  m_changeSelectedNodeSizeAction->setStatusTip(tr("Change size (radius) of the selected nodes"));
  connect(m_changeSelectedNodeSizeAction, &QAction::triggered, this, &ZSwcPack::changeSelectedNodeSize);

  m_setSelectedNodeAsRootAction = new QAction(tr("Set as a root"), this);
  m_setSelectedNodeAsRootAction->setStatusTip(tr("Set the selected node as a root"));
  connect(m_setSelectedNodeAsRootAction, &QAction::triggered, this, &ZSwcPack::setSelectedNodeAsRoot);

  m_contextMenu.addAction(m_deleteSelectedNodesAction);
  m_contextMenu.addAction(m_deleteUnselectedNodesAction);
  m_contextMenu.addAction(m_breakSelectedNodesAction);
  m_contextMenu.addAction(m_connectSelectedNodesAction);
  m_contextMenu.addAction(m_mergeSelectedNodesAction);
  m_contextMenu.addAction(m_insertNodeAction);

  auto* interpolateMenu = m_contextMenu.addMenu(tr("Interpolate"));
  interpolateMenu->addAction(m_interpolateAction);
  interpolateMenu->addAction(m_interpolateZAction);
  interpolateMenu->addAction(m_interpolatePositionAction);
  interpolateMenu->addAction(m_interpolateRadiusAction);

  auto* selectMenu = m_contextMenu.addMenu(tr("Select"));
  selectMenu->addAction(m_selectDownstreamAction);
  selectMenu->addAction(m_selectUpstreamAction);
  selectMenu->addAction(m_selectNeighborAction);
  selectMenu->addAction(m_selectHostBranchAction);
  selectMenu->addAction(m_selectConnectedAction);
  selectMenu->addAction(m_selectAllAction);

  auto* advancedMenu = m_contextMenu.addMenu(tr("Advanced Editing"));
  {
    m_removeTurnAction = new QAction(tr("Remove turn"), advancedMenu);
    m_removeTurnAction->setStatusTip(tr("Remove a nearby sharp turn"));
    connect(m_removeTurnAction, &QAction::triggered, this, &ZSwcPack::removeTurn);
    advancedMenu->addAction(m_removeTurnAction);

    m_resolveCrossoverAction = new QAction(tr("Resolve crossover"), advancedMenu);
    m_resolveCrossoverAction->setStatusTip(tr("Create a crossover near the selected node if it is detected"));
    connect(m_resolveCrossoverAction, &QAction::triggered, this, &ZSwcPack::resolveCrossover);
    advancedMenu->addAction(m_resolveCrossoverAction);

    m_joinIsolatedBranchAction = new QAction(tr("Join isolated branch"), advancedMenu);
    m_joinIsolatedBranchAction->setStatusTip(
      tr("Connect to the nearest branch that does not have a path to the selected nodes"));
    connect(m_joinIsolatedBranchAction, &QAction::triggered, this, &ZSwcPack::joinIsolatedBranch);
    advancedMenu->addAction(m_joinIsolatedBranchAction);

    // Keep the legacy neuTube label (including the original typo) for parity.
    m_joinIsolatedBranchAcrossTreesAction = new QAction(tr("Join isolated brach (across trees)"), advancedMenu);
    m_joinIsolatedBranchAcrossTreesAction->setStatusTip(tr(
      "Connect to the nearest branch that does not have a path to the selected nodes. The branch can be in another neuron."));
    connect(m_joinIsolatedBranchAcrossTreesAction, &QAction::triggered, this, &ZSwcPack::joinIsolatedBranchAcrossTrees);
    advancedMenu->addAction(m_joinIsolatedBranchAcrossTreesAction);

    m_resetBranchPointAction = new QAction(tr("Reset branch point"), advancedMenu);
    m_resetBranchPointAction->setStatusTip(tr("Move a neighboring branch point to the selected node"));
    connect(m_resetBranchPointAction, &QAction::triggered, this, &ZSwcPack::resetBranchPoint);
    advancedMenu->addAction(m_resetBranchPointAction);
  }

  auto* changePropMenu = m_contextMenu.addMenu(tr("Change Property"));
  changePropMenu->addAction(m_translateSelectedNodesAction);
  changePropMenu->addAction(m_changeSelectedNodeSizeAction);
  changePropMenu->addAction(m_setSelectedNodeAsRootAction);

  auto* infoMenu = m_contextMenu.addMenu(tr("Information"));
  infoMenu->addAction(m_showSummaryAction);
  infoMenu->addAction(m_showSelectedBranchLengthAction);
  {
    m_showSelectedBranchScaledLengthAction = new QAction(tr("Scaled Path length"), infoMenu);
    m_showSelectedBranchScaledLengthAction->setStatusTip(tr("Measure overall length with voxel scaling"));
    connect(m_showSelectedBranchScaledLengthAction,
            &QAction::triggered,
            this,
            &ZSwcPack::showSelectedBranchScaledLength);
    infoMenu->addAction(m_showSelectedBranchScaledLengthAction);
  }
}

void ZSwcPack::selectDownstreamNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::set<ZSwc::SwcTreeNode> newSelection = m_selectedNodes;
  for (const auto& n : m_selectedNodes) {
    for (auto it = m_swc.begin(n); it != m_swc.end(n); ++it) {
      newSelection.insert(it);
    }
  }

  setSelectedNodes(newSelection);
}

void ZSwcPack::selectUpstreamNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::set<ZSwc::SwcTreeNode> newSelection = m_selectedNodes;
  for (const auto& n : m_selectedNodes) {
    for (auto it = m_swc.beginAncestor(n); it != m_swc.endAncestor(n); ++it) {
      newSelection.insert(it);
    }
  }

  setSelectedNodes(newSelection);
}

void ZSwcPack::selectNeighborNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::set<ZSwc::SwcTreeNode> newSelection = m_selectedNodes;
  for (const auto& n : m_selectedNodes) {
    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent)) {
      newSelection.insert(parent);
    }
    for (auto it = m_swc.beginChild(n); it != m_swc.endChild(n); ++it) {
      newSelection.insert(it);
    }
  }

  setSelectedNodes(newSelection);
}

void ZSwcPack::selectHostBranchNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::set<ZSwc::SwcTreeNode> newSelection = m_selectedNodes;
  for (const auto& start : m_selectedNodes) {
    auto it = start;
    while (!ZSwc::isNull(it) && !ZSwc::isBranchNode(it)) {
      newSelection.insert(it);
      const auto parent = ZSwc::parent(it);
      if (ZSwc::isNull(parent)) {
        break;
      }
      it = parent;
    }

    it = start;
    while (!ZSwc::isNull(it) && !ZSwc::isBranchNode(it)) {
      newSelection.insert(it);
      const auto child = ZSwc::firstChild(it);
      if (ZSwc::isNull(child)) {
        break;
      }
      it = child;
    }
  }

  setSelectedNodes(newSelection);
}

void ZSwcPack::selectConnectedNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::set<ZSwc::SwcTreeNode> newSelection = m_selectedNodes;
  for (const auto& n : m_selectedNodes) {
    const auto root = ZSwc::root(n);
    if (ZSwc::isNull(root)) {
      continue;
    }
    for (auto it = m_swc.begin(root); it != m_swc.end(root); ++it) {
      newSelection.insert(it);
    }
  }

  setSelectedNodes(newSelection);
}

void ZSwcPack::selectAllNodes()
{
  std::set<ZSwc::SwcTreeNode> newSelection;
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    newSelection.insert(it);
  }
  setSelectedNodes(newSelection);
}

void ZSwcPack::deleteSelectedNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> toDelete;
  toDelete.reserve(m_selectedNodes.size());
  for (auto it = m_swc.beginPostOrder(); it != m_swc.endPostOrder(); ++it) {
    if (it->selected) {
      toDelete.push_back(it);
    }
  }
  if (toDelete.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  deleteNodesLegacyLike(m_swc, toDelete);
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Delete Selected Node"), *this, swcBeforeChange));
}

void ZSwcPack::deleteUnselectedNodes()
{
  if (m_selectedNodes.empty() || m_selectedNodes.size() >= m_swc.size()) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> toDelete;
  toDelete.reserve(m_swc.size() - m_selectedNodes.size());
  for (auto it = m_swc.beginPostOrder(); it != m_swc.endPostOrder(); ++it) {
    if (!it->selected) {
      toDelete.push_back(it);
    }
  }

  if (toDelete.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  deleteNodesLegacyLike(m_swc, toDelete);
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Delete Unselected Node"), *this, swcBeforeChange));
}

void ZSwcPack::showSwcContextMenu(QPoint globalPos)
{
  contextMenu().popup(globalPos);
}

void ZSwcPack::setSelectedNodeAsRoot()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }
  ZSwc swcBeforeChange = m_swc;
  m_swc.setAsRoot(*m_selectedNodes.begin());
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Set as a root"), *this, swcBeforeChange));
}

void ZSwcPack::changeSelectedNodeType()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  const int64_t currentType64 = (*m_selectedNodes.begin())->type;
  const int currentType =
    (currentType64 >= std::numeric_limits<int>::min() && currentType64 <= std::numeric_limits<int>::max())
      ? static_cast<int>(currentType64)
      : 0;

  bool ok = false;
  const int newType = QInputDialog::getInt(QApplication::activeWindow(),
                                           QApplication::applicationName(),
                                           tr("Change type"),
                                           currentType,
                                           0,
                                           std::numeric_limits<int>::max(),
                                           1,
                                           &ok);
  if (!ok) {
    return;
  }

  bool anyChange = false;
  for (const auto& n : m_selectedNodes) {
    if (n->type != newType) {
      anyChange = true;
      break;
    }
  }
  if (!anyChange) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  for (const auto& n : m_selectedNodes) {
    n->type = newType;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Change SWC Node Type"), *this, swcBeforeChange));
}

void ZSwcPack::translateSelectedNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  ZSwcSkeletonTransformDialog dlg(QApplication::activeWindow());
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const double dx = dlg.translateValue(ZSwcSkeletonTransformDialog::X);
  const double dy = dlg.translateValue(ZSwcSkeletonTransformDialog::Y);
  const double dz = dlg.translateValue(ZSwcSkeletonTransformDialog::Z);
  const double sx = dlg.scaleValue(ZSwcSkeletonTransformDialog::X);
  const double sy = dlg.scaleValue(ZSwcSkeletonTransformDialog::Y);
  const double sz = dlg.scaleValue(ZSwcSkeletonTransformDialog::Z);

  if (dx == 0.0 && dy == 0.0 && dz == 0.0 && sx == 1.0 && sy == 1.0 && sz == 1.0) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  for (const auto& n : m_selectedNodes) {
    if (dlg.isTranslateFirst()) {
      n->x += dx;
      n->y += dy;
      n->z += dz;
    }

    n->x *= sx;
    n->y *= sy;
    n->z *= sz;

    if (!dlg.isTranslateFirst()) {
      n->x += dx;
      n->y += dy;
      n->z += dz;
    }
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Translate SWC node"), *this, swcBeforeChange));
}

void ZSwcPack::changeSelectedNodeSize()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  ZSwcSizeDialog dlg(QApplication::activeWindow());
  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const double add = dlg.addValue();
  const double scale = dlg.mulValue();

  if (add == 0.0 && scale == 1.0) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  bool anyChange = false;
  for (const auto& n : m_selectedNodes) {
    const double oldR = n->radius;
    const double newR = std::max(0.0, oldR * scale + add);
    if (newR != oldR) {
      anyChange = true;
      n->radius = newR;
    }
  }

  if (!anyChange) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Node - Change Size"), *this, swcBeforeChange));
}

void ZSwcPack::breakSelectedNodes()
{
  if (m_selectedNodes.size() < 2) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> toReparent;
  toReparent.reserve(m_selectedNodes.size());
  for (const auto& n : m_selectedNodes) {
    const auto parent = ZSwc::parent(n);
    if (ZSwc::isNull(parent) || !parent->selected) {
      continue;
    }
    toReparent.push_back(n);
  }
  if (toReparent.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  for (auto& n : toReparent) {
    // neuTube breaks connections by re-parenting to the virtual master root, which makes
    // the node a new regular root. Atlas does not model that master root, so detach the
    // node to become a forest root.
    m_swc.appendRoot(n);
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Break Connection"), *this, swcBeforeChange));
}

void ZSwcPack::connectSelectedNodes()
{
  if (m_selectedNodes.size() < 2) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> selectedNodes;
  selectedNodes.reserve(m_selectedNodes.size());
  for (const auto& n : m_selectedNodes) {
    selectedNodes.push_back(n);
  }

  ZSwc swcBeforeChange = m_swc;
  if (!connectSelectedNodesLegacyLike(m_swc, selectedNodes)) {
    return;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Connect Swc Nodes"), *this, swcBeforeChange));
}

void ZSwcPack::mergeSelectedNodes()
{
  if (m_selectedNodes.size() <= 1) {
    return;
  }

  // neuTube merge requires the selected nodes to be connected (see zstackdoc.cpp::isMergable()).
  std::set<ZSwc::SwcTreeNode> connected;
  std::vector<ZSwc::SwcTreeNode> queue;
  queue.reserve(m_selectedNodes.size());
  queue.push_back(*m_selectedNodes.begin());
  connected.insert(queue.front());

  for (size_t i = 0; i < queue.size(); ++i) {
    const ZSwc::SwcTreeNode n = queue[i];
    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent) && parent->selected && !connected.contains(parent)) {
      connected.insert(parent);
      queue.push_back(parent);
    }
    for (auto it = m_swc.beginChild(n); it != m_swc.endChild(n); ++it) {
      if (it->selected && !connected.contains(it)) {
        connected.insert(it);
        queue.push_back(it);
      }
    }
  }

  if (connected.size() != m_selectedNodes.size()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             tr("Cannot merge: selected nodes must be directly connected."));
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  std::vector<ZSwc::SwcTreeNode> selectedNodes;
  selectedNodes.reserve(m_selectedNodes.size());
  for (const auto& n : m_selectedNodes) {
    selectedNodes.push_back(n);
  }

  const auto coreOpt = mergeSelectedNodesLegacyLike(m_swc, selectedNodes);
  CHECK(coreOpt.has_value()) << "mergeSelectedNodesLegacyLike returned nullopt for a non-trivial selection";
  const auto coreIt = *coreOpt;

  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    it->selected = false;
  }
  coreIt->selected = true;

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Merge swc nodes"), *this, swcBeforeChange));
}

void ZSwcPack::insertNodesBetweenSelectedPairs()
{
  if (m_selectedNodes.size() < 2) {
    return;
  }

  std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> edges;
  edges.reserve(m_selectedNodes.size());
  for (const auto& n : m_selectedNodes) {
    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent) && parent->selected) {
      edges.emplace_back(parent, n);
    }
  }

  if (edges.empty()) {
    QMessageBox::information(QApplication::activeWindow(),
                             QApplication::applicationName(),
                             tr("Cannot insert: at least two adjacent nodes must be selected."));
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  size_t inserted = 0;
  for (const auto& [parent, child] : edges) {
    SwcNode mid;
    // neuTube `executeInsertSwcNode()` uses `SwcTreeNode::MakePointer()` which defaults
    // id=0 and type=0, then only interpolates XYZ+radius.
    mid.id = 0;
    mid.type = 0;
    mid.x = (parent->x + child->x) * 0.5;
    mid.y = (parent->y + child->y) * 0.5;
    mid.z = (parent->z + child->z) * 0.5;
    mid.radius = (parent->radius + child->radius) * 0.5;
    mid.parentID = -1;
    mid.label = 0;
    mid.weight = 0.0;
    mid.feature = 0.0;
    mid.selected = false;

    const auto midIt = m_swc.appendChild(parent, mid);
    m_swc.appendChild(midIt, child);
    ++inserted;
  }

  if (inserted == 0) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Insert SWC Node"), *this, swcBeforeChange));
}

namespace {

enum class InterpMode
{
  Z,
  Position,
  Radius,
  All
};

[[nodiscard]] bool isContinuationNode(const ZSwc& swc, const ZSwc::SwcTreeNode& n)
{
  if (ZSwc::isNull(n) || ZSwc::isRoot(n)) {
    return false;
  }
  if (ZSwc::isBranchNode(n)) {
    return false;
  }
  return swc.numChildren(n) == 1;
}

[[nodiscard]] double planePathLengthUp(ZSwc::SwcTreeNode from, const ZSwc::SwcTreeNode& toAncestor)
{
  double len = 0.0;
  while (!ZSwc::isNull(from) && from != toAncestor) {
    const auto p = ZSwc::parent(from);
    CHECK(!ZSwc::isNull(p));
    len += dist2D(*from, *p);
    from = p;
  }
  return len;
}

[[nodiscard]] double planePathLengthDown(ZSwc::SwcTreeNode from, const ZSwc::SwcTreeNode& toDesc)
{
  double len = 0.0;
  while (!ZSwc::isNull(from) && from != toDesc) {
    const auto c = ZSwc::firstChild(from);
    CHECK(!ZSwc::isNull(c));
    len += dist2D(*from, *c);
    from = c;
  }
  return len;
}

struct InterpUpdate
{
  ZSwc::SwcTreeNode node;
  glm::dvec3 pos;
  double radius = 0.0;
};

[[nodiscard]] std::vector<InterpUpdate>
computeInterpolationUpdates(const ZSwc& swc, const std::set<ZSwc::SwcTreeNode>& selectedNodes, InterpMode mode)
{
  std::vector<InterpUpdate> updates;
  updates.reserve(selectedNodes.size());

  for (const auto& n : selectedNodes) {
    if (!isContinuationNode(swc, n)) {
      continue;
    }

    ZSwc::SwcTreeNode upEnd = ZSwc::parent(n);
    while (isContinuationNode(swc, upEnd) && upEnd->selected) {
      upEnd = ZSwc::parent(upEnd);
    }
    ZSwc::SwcTreeNode downEnd = ZSwc::firstChild(n);
    while (isContinuationNode(swc, downEnd) && downEnd->selected) {
      downEnd = ZSwc::firstChild(downEnd);
    }

    const double dist1 = planePathLengthUp(n, upEnd);
    const double dist2 = planePathLengthDown(n, downEnd);
    const double denom = dist1 + dist2;
    const double lambda = (denom == 0.0) ? 0.0 : (dist1 / denom);

    InterpUpdate u;
    u.node = n;
    u.pos = glm::dvec3(n->x, n->y, n->z);
    u.radius = n->radius;

    switch (mode) {
      case InterpMode::Z: {
        u.pos.z = (denom == 0.0) ? upEnd->z : (upEnd->z * (1.0 - lambda) + downEnd->z * lambda);
        break;
      }
      case InterpMode::Position: {
        if (denom == 0.0) {
          u.pos = glm::dvec3(upEnd->x, upEnd->y, upEnd->z);
        } else {
          u.pos = glm::dvec3(upEnd->x, upEnd->y, upEnd->z) * (1.0 - lambda) +
                  glm::dvec3(downEnd->x, downEnd->y, downEnd->z) * lambda;
        }
        break;
      }
      case InterpMode::Radius: {
        u.radius = (denom == 0.0) ? upEnd->radius : (upEnd->radius * (1.0 - lambda) + downEnd->radius * lambda);
        break;
      }
      case InterpMode::All: {
        if (denom == 0.0) {
          u.pos = glm::dvec3(upEnd->x, upEnd->y, upEnd->z);
          u.radius = upEnd->radius;
        } else {
          u.pos = glm::dvec3(upEnd->x, upEnd->y, upEnd->z) * (1.0 - lambda) +
                  glm::dvec3(downEnd->x, downEnd->y, downEnd->z) * lambda;
          u.radius = upEnd->radius * (1.0 - lambda) + downEnd->radius * lambda;
        }
        break;
      }
    }

    updates.push_back(u);
  }

  return updates;
}

} // namespace

void ZSwcPack::interpolateSelectedNodes()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  const std::vector<InterpUpdate> updates = computeInterpolationUpdates(m_swc, m_selectedNodes, InterpMode::All);
  if (updates.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  size_t changed = 0;
  for (const auto& u : updates) {
    if (u.node->x != u.pos.x || u.node->y != u.pos.y || u.node->z != u.pos.z || u.node->radius != u.radius) {
      u.node->x = u.pos.x;
      u.node->y = u.pos.y;
      u.node->z = u.pos.z;
      u.node->radius = u.radius;
      ++changed;
    }
  }
  if (changed == 0) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Interpolation"), *this, swcBeforeChange));
}

void ZSwcPack::interpolateSelectedNodesZ()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  const std::vector<InterpUpdate> updates = computeInterpolationUpdates(m_swc, m_selectedNodes, InterpMode::Z);
  if (updates.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  size_t changed = 0;
  for (const auto& u : updates) {
    if (u.node->z != u.pos.z) {
      u.node->z = u.pos.z;
      ++changed;
    }
  }
  if (changed == 0) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Z Interpolation"), *this, swcBeforeChange));
}

void ZSwcPack::interpolateSelectedNodesPosition()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  const std::vector<InterpUpdate> updates = computeInterpolationUpdates(m_swc, m_selectedNodes, InterpMode::Position);
  if (updates.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  size_t changed = 0;
  for (const auto& u : updates) {
    if (u.node->x != u.pos.x || u.node->y != u.pos.y || u.node->z != u.pos.z) {
      u.node->x = u.pos.x;
      u.node->y = u.pos.y;
      u.node->z = u.pos.z;
      ++changed;
    }
  }
  if (changed == 0) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Position Interpolation"), *this, swcBeforeChange));
}

void ZSwcPack::interpolateSelectedNodesRadius()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  const std::vector<InterpUpdate> updates = computeInterpolationUpdates(m_swc, m_selectedNodes, InterpMode::Radius);
  if (updates.empty()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  size_t changed = 0;
  for (const auto& u : updates) {
    if (u.node->radius != u.radius) {
      u.node->radius = u.radius;
      ++changed;
    }
  }
  if (changed == 0) {
    return;
  }
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Radius Interpolation"), *this, swcBeforeChange));
}

void ZSwcPack::showSwcSummary()
{
  ZInformationDialog dlg(QApplication::activeWindow());

  if (m_swc.empty()) {
    dlg.setText(QStringLiteral("<p>No neuron data.</p>"));
    dlg.exec();
    return;
  }

  double length = 0.0;
  for (auto it = m_swc.begin(); it != m_swc.end(); ++it) {
    const auto parent = ZSwc::parent(it);
    if (!ZSwc::isNull(parent)) {
      length += dist3D(*it, *parent);
    }
  }

  dlg.setText(QStringLiteral("<p>Overall length of 1 Neuron(s): %1</p>").arg(QString::number(length)));
  dlg.exec();
}

void ZSwcPack::showSelectedBranchLength()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  double segmentLength = 0.0;
  for (const auto& n : m_selectedNodes) {
    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent) && parent->selected) {
      segmentLength += dist3D(*n, *parent);
    }
  }

  QString html = QStringLiteral("<p>Overall length of selected branches: %1</p>").arg(QString::number(segmentLength));
  if (m_selectedNodes.size() == 2) {
    auto it = m_selectedNodes.begin();
    const auto n1 = *it;
    ++it;
    const auto n2 = *it;
    if (!ZSwc::inSameTree(n1, n2)) {
      const double d = dist3D(*n1, *n2);
      html +=
        QStringLiteral("<p>Straight line distance between the two selected nodes: %1</p>").arg(QString::number(d));
    }
  }

  ZInformationDialog dlg(QApplication::activeWindow());
  dlg.setText(html);
  dlg.exec();
}

void ZSwcPack::showSelectedBranchScaledLength()
{
  if (m_selectedNodes.empty()) {
    return;
  }

  ZResolutionDialog resDlg(QApplication::activeWindow());
  if (resDlg.exec() != QDialog::Accepted) {
    return;
  }

  const double sx = resDlg.xScale();
  const double sy = resDlg.yScale();
  const double sz = resDlg.zScale();

  auto scaledDist3D = [&](const SwcNode& a, const SwcNode& b) -> double {
    const double dx = (a.x - b.x) * sx;
    const double dy = (a.y - b.y) * sy;
    const double dz = (a.z - b.z) * sz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };

  double segmentLength = 0.0;
  for (const auto& n : m_selectedNodes) {
    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent) && parent->selected) {
      segmentLength += scaledDist3D(*n, *parent);
    }
  }

  QString html = QStringLiteral("<p>Overall length of selected branches: %1</p>").arg(QString::number(segmentLength));

  if (m_selectedNodes.size() == 2) {
    auto it = m_selectedNodes.begin();
    const auto n1 = *it;
    ++it;
    const auto n2 = *it;
    if (!ZSwc::inSameTree(n1, n2)) {
      const double d = scaledDist3D(*n1, *n2);
      html +=
        QStringLiteral("<p>Straight line distance between the two selected nodes: %1</p>").arg(QString::number(d));
    }
  }

  ZInformationDialog dlg(QApplication::activeWindow());
  dlg.setText(html);
  dlg.exec();
}

void ZSwcPack::removeTurn()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const auto node = *m_selectedNodes.begin();
  const auto updated = computeRemoveTurnGeometryLegacyLike(m_swc, node);
  if (!updated.has_value()) {
    return;
  }

  ZSwc swcBeforeChange = m_swc;
  node->x = updated->x;
  node->y = updated->y;
  node->z = updated->z;
  node->radius = updated->radius;
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Remove turn"), *this, swcBeforeChange));
}

void ZSwcPack::resolveCrossover()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const auto center = *m_selectedNodes.begin();
  ZSwc swcBeforeChange = m_swc;
  if (!resolveCrossoverLegacyLike(m_swc, center)) {
    return;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Resolve crossover"), *this, swcBeforeChange));
}

void ZSwcPack::joinIsolatedBranch()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const auto branchPoint = *m_selectedNodes.begin();
  ZSwc swcBeforeChange = m_swc;
  if (!joinIsolatedBranchLegacyLike(m_swc, branchPoint)) {
    return;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Set branch point"), *this, swcBeforeChange));
}

void ZSwcPack::joinIsolatedBranchAcrossTrees()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  // neuTube: allow connecting to an isolated branch in another tree under the same SWC forest, or in another SWC
  // object.
  const auto branchPoint = *m_selectedNodes.begin();
  if (ZSwc::isNull(branchPoint)) {
    return;
  }

  if (m_swc.numRoots() <= 1 && m_doc.objs().size() <= 1) {
    // No other tree to connect to (within this pack or in another SWC pack).
    return;
  }

  struct Candidate
  {
    ZSwcPack* pack = nullptr;
    ZSwc::SwcTreeNode node{};
    double dist = std::numeric_limits<double>::infinity();
  };

  Candidate best;
  auto consider = [&](ZSwcPack* pack, const ZSwc::SwcTreeNode& node) {
    if (pack == nullptr || ZSwc::isNull(node) || ZSwc::isNull(branchPoint)) {
      return;
    }
    if (node->id < 0) { // Match legacy SwcTreeNode::isRegular().
      return;
    }

    const double d = swcNodeSurfaceDistanceLegacyLike(node, branchPoint);
    if (d < best.dist) {
      best.pack = pack;
      best.node = node;
      best.dist = d;
    }
  };

  // 1) Search within this SWC (other roots only).
  if (m_swc.numRoots() > 1) {
    const ZSwc::SwcTreeNode hostRoot = ZSwc::root(branchPoint);
    for (auto rit = m_swc.beginRoot(); rit != m_swc.endRoot(); ++rit) {
      const ZSwc::SwcTreeNode root = ZSwc::SwcTreeNode(rit);
      if (root == hostRoot) {
        continue;
      }
      for (auto it = m_swc.begin(root); it != m_swc.end(root); ++it) {
        consider(this, it);
      }
    }
  }

  // 2) Search in other SWC packs (other neurons).
  std::set<ZSwcPack*> visitedPacks;
  for (const size_t id : m_doc.objs()) {
    ZSwcPack& pack = m_doc.swcPack(id);
    if (&pack == this) {
      continue;
    }
    if (visitedPacks.contains(&pack)) { // Skip aliases pointing at the same pack.
      continue;
    }
    visitedPacks.insert(&pack);

    for (auto it = pack.m_swc.begin(); it != pack.m_swc.end(); ++it) {
      consider(&pack, it);
    }
  }

  if (best.pack == nullptr || ZSwc::isNull(best.node)) {
    return;
  }

  if (best.pack == this) {
    ZSwc swcBeforeChange = m_swc;
    auto closest = best.node;
    if (!ZSwc::isRoot(closest)) {
      m_swc.setAsRoot(closest);
    }
    m_swc.appendChild(branchPoint, closest);
    m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Connect isolated SWC"), *this, swcBeforeChange));
    return;
  }

  // Cross-pack merge: move the closest branch (re-rooted at the closest node) from the other pack into this pack.
  ZSwcPack* other = best.pack;
  CHECK(other);
  ZSwc swcBeforeThis = m_swc;
  ZSwc swcBeforeOther = other->m_swc;

  auto closestOther = best.node;
  if (!ZSwc::isRoot(closestOther)) {
    other->m_swc.setAsRoot(closestOther);
  }

  // Copy the entire re-rooted subtree into this pack.
  const auto movedRoot = m_swc.appendRoot(*closestOther);
  m_swc.copy(movedRoot, other->m_swc, closestOther);
  m_swc.appendChild(branchPoint, movedRoot);

  // Remove the moved subtree from the source pack.
  other->m_swc.eraseSubtree(closestOther);

  // Record undo on both packs. Atlas uses per-object undo stacks, so cross-pack edits require undoing each pack.
  other->m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Connect isolated SWC"), *other, swcBeforeOther));
  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Connect isolated SWC"), *this, swcBeforeThis));
}

void ZSwcPack::resetBranchPoint()
{
  if (m_selectedNodes.size() != 1) {
    return;
  }

  const auto loopNode = *m_selectedNodes.begin();
  ZSwc swcBeforeChange = m_swc;
  if (!resetBranchPointLegacyLike(m_swc, loopNode)) {
    return;
  }

  m_undoStack.push(new ZSwcEditCommand(QStringLiteral("Reset branch point"), *this, swcBeforeChange));
}

} // namespace nim
