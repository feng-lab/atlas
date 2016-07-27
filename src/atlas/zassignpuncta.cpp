#include "zassignpuncta.h"

#include "zimg.h"
#include "zlog.h"
#include "zglmutils.h"
#include "zimggraph.h"
#include "zimgautothreshold.h"

namespace nim {

ZAssignPuncta::ZAssignPuncta(const ZImg& img, size_t dendriteChannel, size_t t)
  : ZImgProcess()
  , m_img(img)
  , m_dendriteChannel(dendriteChannel)
  , m_t(t)
  , m_maxDistToBranch(2.5)
  , m_ambiguousFactor(1.0)
{
  if (!m_img.isType<uint8_t>()) {
    throw ZImgException("puncta assign only support uint8_t img");
  }
}

ZAssignPuncta::~ZAssignPuncta()
{
  clearAllSwcTrees();
}

void ZAssignPuncta::clearAllSwcTrees()
{
  m_swcTreeToPuncta.clear();
  m_swcTreeToSomaPuncta.clear();
}

void ZAssignPuncta::addSwcTree(ZSwc *tree)
{
  assert(tree);
  m_swcTreeToPuncta[tree] = ZPuncta();
  m_swcTreeToSomaPuncta[tree] = ZPuncta();
}

void ZAssignPuncta::addSwcTrees(const std::vector<ZSwc*> &trees)
{
  for (size_t i=0; i<trees.size(); ++i)
    addSwcTree(trees[i]);
}

void ZAssignPuncta::addSwcTrees(std::vector<ZSwc> &trees)
{
  for (size_t i=0; i<trees.size(); ++i)
    addSwcTree(&trees[i]);
}

void ZAssignPuncta::doWork()
{
  if (m_img.info().voxelSizeUnit == VoxelSizeUnit::none) {
    throw ZImgException("Voxel Size not set, Abort Assign Puncta.");
  }
  if (m_dendriteChannel < m_img.numChannels()) {
    LOG(INFO) << "Start Assign Puncta.";
    LOG(INFO) << "Dendrite Channel: " << m_dendriteChannel+1 << " (start from 1)";
    LOG(INFO) << "Voxel Size X: " << m_img.voxelSizeXInUm() << " um";
    LOG(INFO) << "Voxel Size Y: " << m_img.voxelSizeYInUm() << " um";
    LOG(INFO) << "Voxel Size Z: " << m_img.voxelSizeZInUm() << " um";
    LOG(INFO) << "Max Distance To Branch: " << m_maxDistToBranch << " um";
    LOG(INFO) << "Ambiguous Factor: " << m_ambiguousFactor;
    LOG(INFO) << "Number of Puncta: " << m_puncta.size();
    LOG(INFO) << "Number of Soma Puncta: " << m_somaPuncta.size();
    LOG(INFO) << "Number of Swc Trees: " << m_swcTreeToPuncta.size();
  } else {
    throw ZImgException(QString("Wrong dendrite channel: %1. Abort.").arg(m_dendriteChannel));
  }
  for (auto it = m_swcTreeToPuncta.begin(); it != m_swcTreeToPuncta.end(); ++it) {
    it->first->labelSomaAndOthers(3.0 / m_img.voxelSizeXInUm()); // soma radius at least 3um
    it->second.clear();
  }
  for (auto it = m_swcTreeToSomaPuncta.begin(); it != m_swcTreeToSomaPuncta.end(); ++it) {
    it->second.clear();
  }
  m_ambiguousPuncta.clear();

  //
  if (m_swcTreeToPuncta.size() > 0) {
    separateSomaPuncta();
    separatePuncta();
  }

  LOG(INFO) << "End Assign Puncta.";
}

ZPuncta ZAssignPuncta::getPunctaOfTree(ZSwc *tree) const
{
  std::map<ZSwc*,ZPuncta>::const_iterator it;
  it = m_swcTreeToPuncta.find(tree);
  if (it != m_swcTreeToPuncta.end()) {
    return it->second;
  } else {
    throw ZImgException("getPunctaOfTree: Input tree not found.");
  }
}

ZPuncta ZAssignPuncta::getSomaPunctaOfTree(ZSwc *tree) const
{
  std::map<ZSwc*,ZPuncta>::const_iterator it;
  it = m_swcTreeToSomaPuncta.find(tree);
  if (it != m_swcTreeToSomaPuncta.end()) {
    return it->second;
  } else {
    throw ZImgException("getSomaPunctaOfTree: Input tree not found.");
  }
}

void ZAssignPuncta::separatePuncta()
{
  if (m_puncta.isEmpty())
    reportProgress(1.0);
  std::map<SwcTreeNode,ZSwc*> nodeToTree;
  std::map<ZSwc*,ZPuncta>::const_iterator it;
  double punctaSize = m_puncta.size();
  size_t idx = 1;
  for (ZPuncta::const_iterator pit = m_puncta.begin(); pit != m_puncta.end(); ++pit) {
    LOG(INFO) << "Start Puncta " << idx;
    nodeToTree.clear();
    std::vector<SwcTreeNode> nodes;
    size_t numTreeInRange = 0;
    for (it = m_swcTreeToPuncta.begin(); it != m_swcTreeToPuncta.end(); ++it) {
      std::vector<SwcTreeNode> tmpNodes = nodesNearbyPuncta(*pit, it->first);
      if (!tmpNodes.empty()) {
        ++numTreeInRange;
        for (size_t tmpNodesIdx = 0; tmpNodesIdx < tmpNodes.size(); ++tmpNodesIdx) {
          nodeToTree[tmpNodes[tmpNodesIdx]] = it->first;
          nodes.push_back(tmpNodes[tmpNodesIdx]);
        }
      }
    }

    if (numTreeInRange == 1) {
      m_swcTreeToPuncta[nodeToTree.begin()->second].push_back(*pit);
    } else if (numTreeInRange > 1) {
      bool isAmbiguous = false;
      SwcTreeNode tn = intensityWeightedNearestNode(pit->x(), pit->y(), pit->z(),
                                                    nodes, isAmbiguous);
      if (nearestNode(pit->x(), pit->y(), pit->z(), nodes) != tn) {
        LOG(WARNING) << "Check Punctum: "
                     << pit->x() << " " << pit->y() << " " << pit->z() << " " << pit->radius() << " "
                     << pit->maxIntensity() << " " << pit->meanIntensity();
      }
      if (isAmbiguous) {
        m_ambiguousPuncta.push_back(*pit);
      } else {
        m_swcTreeToPuncta[nodeToTree[tn]].push_back(*pit);
      }
    }
    reportProgress(0.25 + 0.75 * idx / punctaSize);
    ++idx;
  }
}

void ZAssignPuncta::separateSomaPuncta()
{
  if (m_somaPuncta.isEmpty())
    reportProgress(.25);
  std::map<ZSwc*,ZPuncta>::const_iterator it;
  double punctaSize = m_somaPuncta.size();
  size_t idx = 1;
  for (ZPuncta::const_iterator pit = m_somaPuncta.begin(); pit != m_somaPuncta.end(); ++pit) {
    LOG(INFO) << "Start Soma Puncta " << idx;
    double min_dist = std::numeric_limits<double>::max();
    ZSwc *tree = nullptr;
    for (it = m_swcTreeToSomaPuncta.begin(); it != m_swcTreeToSomaPuncta.end(); ++it) {
      double dist = punctaSomaDist(*pit, it->first);
      if (dist < min_dist) {
        min_dist = dist;
        tree = it->first;
      }
    }
    if (tree) {
      m_swcTreeToSomaPuncta[tree].push_back(*pit);
    }
    reportProgress(.25 * idx / punctaSize);
    idx++;
  }
}

double ZAssignPuncta::punctaTreeDist(const ZPunctum &punctum, ZSwc *tree, SwcTreeNode &nearestNode) const
{
  nearestNode = SwcTreeNode();
  double min_dist = std::numeric_limits<double>::max();
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_img.voxelSizeXInUm(), m_img.voxelSizeYInUm(), m_img.voxelSizeZInUm());
  pt *= res;
  for (SwcTreeNode tn = tree->begin(); tn != tree->end(); ++tn) {
    if (!ZSwc::isRoot(tn) && tn->type != m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      double dist = glm::length(node - pt);
      if (dist > 100) {
        continue;
      }
      dist = pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), tn, ZSwc::parent(tn));
      if (dist < m_maxDistToBranch + punctum.radius() * m_img.voxelSizeXInUm() && dist < min_dist) {   //in mask area
        min_dist = dist;
        nearestNode = tn;
      }
    }
  }
  return min_dist;
}

std::vector<ZAssignPuncta::SwcTreeNode> ZAssignPuncta::nodesNearbyPuncta(const ZPunctum &punctum, ZSwc *tree) const
{
  std::vector<SwcTreeNode> nodes;
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_img.voxelSizeXInUm(), m_img.voxelSizeYInUm(), m_img.voxelSizeZInUm());
  pt *= res;
  for (SwcTreeNode tn = tree->begin(); tn != tree->end(); ++tn) {
    if (!ZSwc::isRoot(tn) && tn->type != m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      double dist = glm::length(node - pt);
      if (dist > 100) {
        continue;
      }
      dist = pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), tn, ZSwc::parent(tn));
      if (dist < m_maxDistToBranch + punctum.radius() * m_img.voxelSizeXInUm()) {   //in mask area
        nodes.push_back(tn);
      }
    }
  }
  return nodes;
}

double ZAssignPuncta::punctaSomaDist(const ZPunctum &punctum, ZSwc *tree) const
{
  double min_dist = std::numeric_limits<double>::max();
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_img.voxelSizeXInUm(), m_img.voxelSizeYInUm(), m_img.voxelSizeZInUm());
  pt *= res;
  for (SwcTreeNode tn = tree->begin(); tn != tree->end(); ++tn) {
    if (ZSwc::isRoot(tn) || tn->type == m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      double dist = glm::length(node - pt);
      // typical soma diameter would be 10-15um
      if (dist < 20 && dist < min_dist) {
        min_dist = dist;
      }
    }
  }
  return min_dist;
}

double ZAssignPuncta::pointFrustumConeDist(double x, double y, double z, const SwcTreeNode &tn, const SwcTreeNode &ptn) const
{
  double dist;
  glm::dvec3 pt(x,y,z);
  glm::dvec3 bot(tn->x, tn->y, tn->z);
  glm::dvec3 top(ptn->x, ptn->y, ptn->z);
  glm::dvec3 res(m_img.voxelSizeXInUm(), m_img.voxelSizeYInUm(), m_img.voxelSizeZInUm());
  pt *= res;
  bot *= res;
  top *= res;
  double normtb = glm::dot(top - bot, top - bot);
  double normbp = glm::dot(bot - pt, bot - pt);
  double normtp = glm::dot(top - pt, top - pt);
  double dotbptb = glm::dot(bot - pt, top - bot);
  double frac = -dotbptb / normtb;
  if (frac < 0)
    dist = std::sqrt(normbp) - tn->radius * m_img.voxelSizeXInUm();
  else if (frac > 1)
    dist = std::sqrt(normtp) - ptn->radius * m_img.voxelSizeXInUm();
  else {
    double radius = m_img.voxelSizeXInUm() * ((1-frac)*tn->radius + frac*ptn->radius);
    dist = std::sqrt(normbp - dotbptb * dotbptb / normtb) - radius;
  }
  return dist;
}

ZAssignPuncta::SwcTreeNode ZAssignPuncta::intensityWeightedNearestNode(double x, double y, double z,
                                                                       const std::vector<SwcTreeNode> &nodes, bool &isAmbiguous)
{
  //first crop out the region
  int left = roundTo<int>(x);
  int right = roundTo<int>(x);
  int up = roundTo<int>(y);
  int down = roundTo<int>(y);
  int zup = roundTo<int>(z);
  int zdown = roundTo<int>(z);
  for (size_t i=0; i<nodes.size(); ++i) {
    SwcTreeNode parent = ZSwc::parent(nodes[i]);
    left = std::min(std::min(left, roundTo<int>(nodes[i]->x)), roundTo<int>(parent->x));
    right = std::max(std::max(right, roundTo<int>(nodes[i]->x)), roundTo<int>(parent->x));
    up = std::min(std::min(up, roundTo<int>(nodes[i]->y)), roundTo<int>(parent->y));
    down = std::max(std::max(down, roundTo<int>(nodes[i]->y)), roundTo<int>(parent->y));
    zup = std::min(std::min(zup, roundTo<int>(nodes[i]->z)), roundTo<int>(parent->z));
    zdown = std::max(std::max(zdown, roundTo<int>(nodes[i]->z)), roundTo<int>(parent->z));
  }

  left = std::max(0, left);
  right = std::min(right, static_cast<int>(m_img.width())-1);
  up = std::max(0, up);
  down = std::min(down, static_cast<int>(m_img.height())-1);
  zup = std::max(0, zup);
  zdown = std::min(zdown, static_cast<int>(m_img.depth())-1);
  ZImgRegion rgn(left, right+1, up, down+1, zup, zdown+1, m_dendriteChannel, m_dendriteChannel+1, m_t, m_t+1);
  ZImg img = m_img.crop(rgn);

  ZImgGraph imgGraph(img);
  imgGraph.setConnectivity(26);
  ZImgAutoThreshold<> imgAutoThre;
  double cent1 = 0;
  double cent2 = 0;
  double thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, img);
  double scale = cent2 - cent1;
  if (scale < 1.0)
    scale = 1.0;
  scale /= 9.2;
  imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale));

  ZVoxelCoordinate startCoord(roundTo<int>(x)-left, roundTo<int>(y)-up, roundTo<int>(z)-zup);
  std::vector<double> dist = imgGraph.shortestPaths(startCoord);

  std::vector<double> nodeMinDists(nodes.size(), std::numeric_limits<double>::max());
  for (size_t v=0; v<dist.size(); ++v) {
    ZVoxelCoordinate coord = img.indexToCoord(v);
    int nodeIdx = -1;
    for (int i=nodes.size()-1; i>=0; --i) {
      if (pointFrustumConeDist(coord.x+left, coord.y+up, coord.z+zup, nodes[i], ZSwc::parent(nodes[i])) <= 0.0) {
        nodeIdx = i;
        break;
      }
    }
    if (nodeIdx > -1)
      nodeMinDists[nodeIdx] = std::min(nodeMinDists[nodeIdx], dist[v]);
  }

  size_t minIndex = std::min_element(nodeMinDists.begin(), nodeMinDists.end()) - nodeMinDists.begin();
  double minDist = nodeMinDists[minIndex];
  nodeMinDists[minIndex] = std::numeric_limits<double>::max();
  double secondMinDist = *std::min_element(nodeMinDists.begin(), nodeMinDists.end());
  //LOG(INFO) << " min dist " << minDist;
  //LOG(INFO) << " second min dist " << secondMinDist;
  isAmbiguous = secondMinDist < m_ambiguousFactor*minDist;

  return nodes[minIndex];
}

ZAssignPuncta::SwcTreeNode ZAssignPuncta::nearestNode(double x, double y, double z, const std::vector<SwcTreeNode> &nodes)
{
  double dist = std::numeric_limits<double>::max();
  SwcTreeNode res;
  for (size_t i=0; i<nodes.size(); ++i) {
    const SwcTreeNode &node = nodes[i];
    double nodeDist = pointFrustumConeDist(x,y,z,node,ZSwc::parent(node));
    if (nodeDist < dist) {
      dist = nodeDist;
      res = node;
    }
  }
  assert(!ZSwc::isNull(res));
  return res;
}

} // namespace nim
