#include "zassignpuncta.h"

#include <utility>

#include "zimg.h"
#include "zlog.h"
#include "zglmutils.h"
#include "zimggraph.h"
#include "zimgautothreshold.h"

namespace nim {

ZAssignPuncta::ZAssignPuncta(const ZImg& img, size_t dendriteChannel, size_t t)
  : m_img(&img)
  , m_dendriteChannel(dendriteChannel)
  , m_t(t)
{
  if (!m_img->isType<uint8_t>()) {
    throw ZException("puncta assign only support uint8_t img");
  }
  m_imgInfo = m_img->info();
}

ZAssignPuncta::ZAssignPuncta(QString filename,
                             double minValue,
                             double maxValue,
                             size_t dendriteChannel,
                             size_t t,
                             size_t scene)
  : m_filename(std::move(filename))
  , m_minValue(minValue)
  , m_maxValue(maxValue)
  , m_dendriteChannel(dendriteChannel)
  , m_t(t)
  , m_scene(scene)
{
  if (auto infos = ZImg::readImgInfos(m_filename); m_scene >= infos.size()) {
    throw ZException("invalid scene");
  }
}

ZAssignPuncta::ZAssignPuncta(QString filename,
                             const ZImgInfo& imgInfo,
                             double minValue,
                             double maxValue,
                             size_t dendriteChannel,
                             size_t t,
                             size_t scene)
  : ZAssignPuncta(std::move(filename), minValue, maxValue, dendriteChannel, t, scene)
{
  auto infos = ZImg::readImgInfos(m_filename);
  m_imgInfo.swap(infos[scene]);
  m_imgInfo.voxelSizeUnit = imgInfo.voxelSizeUnit;
  m_imgInfo.voxelSizeX = imgInfo.voxelSizeX;
  m_imgInfo.voxelSizeY = imgInfo.voxelSizeY;
  m_imgInfo.voxelSizeZ = imgInfo.voxelSizeZ;
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

void ZAssignPuncta::addSwcTree(const ZSwc* tree)
{
  CHECK(tree);
  m_swcTreeToPuncta[tree] = ZPuncta();
  m_swcTreeToSomaPuncta[tree] = ZPuncta();
}

void ZAssignPuncta::addSwcTrees(const std::vector<ZSwc>& trees)
{
  for (const auto& tree : trees) {
    addSwcTree(&tree);
  }
}

void ZAssignPuncta::doWork()
{
  if (m_imgInfo.voxelSizeUnit == VoxelSizeUnit::none) {
    throw ZException("Voxel Size not set, Abort Assign Puncta.");
  }
  if (m_dendriteChannel < m_imgInfo.numChannels) {
    LOG(INFO) << "Start Assign Puncta.";
    LOG(INFO) << "Dendrite Channel: " << m_dendriteChannel + 1 << " (start from 1)";
    LOG(INFO) << "Voxel Size X: " << m_imgInfo.voxelSizeXInUm() << " um";
    LOG(INFO) << "Voxel Size Y: " << m_imgInfo.voxelSizeYInUm() << " um";
    LOG(INFO) << "Voxel Size Z: " << m_imgInfo.voxelSizeZInUm() << " um";
    LOG(INFO) << "Max Distance To Branch: " << m_maxDistToBranch << " um";
    LOG(INFO) << "Ambiguous Factor: " << m_ambiguousFactor;
    LOG(INFO) << "Number of Puncta: " << m_puncta.data.size();
    LOG(INFO) << "Number of Soma Puncta: " << m_somaPuncta.data.size();
    LOG(INFO) << "Number of Swc Trees: " << m_swcTreeToPuncta.size();
  } else {
    throw ZException(fmt::format("Wrong dendrite channel: {}. Abort.", m_dendriteChannel));
  }
  for (auto& treePuncta : m_swcTreeToPuncta) {
    treePuncta.second.clear();
  }
  for (auto& treePuncta : m_swcTreeToSomaPuncta) {
    treePuncta.second.clear();
  }
  m_ambiguousPuncta.clear();

  //
  if (!m_swcTreeToPuncta.empty()) {
    separateSomaPuncta();
    separatePuncta();
  }

  LOG(INFO) << "End Assign Puncta.";
}

ZPuncta ZAssignPuncta::getPunctaOfTree(const ZSwc* tree) const
{
  if (auto it = m_swcTreeToPuncta.find(tree); it != m_swcTreeToPuncta.end()) {
    return it->second;
  } else {
    throw ZException("getPunctaOfTree: Input tree not found.");
  }
}

ZPuncta ZAssignPuncta::getSomaPunctaOfTree(const ZSwc* tree) const
{
  if (auto it = m_swcTreeToSomaPuncta.find(tree); it != m_swcTreeToSomaPuncta.end()) {
    return it->second;
  } else {
    throw ZException("getSomaPunctaOfTree: Input tree not found.");
  }
}

void ZAssignPuncta::separatePuncta()
{
  if (m_puncta.data.empty()) {
    reportProgress(1.0);
  }
  std::map<ZSwc::ConstSwcTreeNode, const ZSwc*> nodeToTree;
  double punctaSize = m_puncta.data.size();
  size_t idx = 1;
  for (const auto& p : m_puncta.data) {
    LOG(INFO) << "Start Puncta " << idx;
    nodeToTree.clear();
    std::vector<ZSwc::ConstSwcTreeNode> nodes;
    size_t numTreeInRange = 0;
    for (const auto& treePuncta : m_swcTreeToPuncta) {
      if (std::vector<ZSwc::ConstSwcTreeNode> tmpNodes = nodesNearbyPuncta(p, treePuncta.first); !tmpNodes.empty()) {
        ++numTreeInRange;
        for (auto tmpNode : tmpNodes) {
          nodeToTree[tmpNode] = treePuncta.first;
          nodes.push_back(tmpNode);
        }
      }
    }

    if (numTreeInRange == 1) {
      m_swcTreeToPuncta[nodeToTree.begin()->second].data.push_back(p);
    } else if (numTreeInRange > 1) {
      bool isAmbiguous = false;
      ZSwc::ConstSwcTreeNode tn = intensityWeightedNearestNode(p.x(), p.y(), p.z(), nodes, isAmbiguous);
      if (nearestNode(p.x(), p.y(), p.z(), nodes) != tn) {
        LOG(WARNING) << "Check Punctum: " << p.x() << " " << p.y() << " " << p.z() << " " << p.radius() << " "
                     << p.maxIntensity() << " " << p.meanIntensity();
      }
      if (isAmbiguous) {
        m_ambiguousPuncta.data.push_back(p);
      } else {
        m_swcTreeToPuncta[nodeToTree[tn]].data.push_back(p);
      }
    }
    reportProgress(0.25 + 0.75 * idx / punctaSize);
    ++idx;
  }
}

void ZAssignPuncta::separateSomaPuncta()
{
  if (m_somaPuncta.data.empty()) {
    reportProgress(.25);
  }
  double punctaSize = m_somaPuncta.data.size();
  size_t idx = 1;
  for (const auto& p : m_somaPuncta.data) {
    LOG(INFO) << "Start Soma Puncta " << idx;
    double min_dist = std::numeric_limits<double>::max();
    const ZSwc* tree = nullptr;
    for (const auto& treePuncta : m_swcTreeToSomaPuncta) {
      if (double dist = punctaSomaDist(p, treePuncta.first); dist < min_dist) {
        min_dist = dist;
        tree = treePuncta.first;
      }
    }
    if (tree) {
      m_swcTreeToSomaPuncta[tree].data.push_back(p);
    }
    reportProgress(.25 * idx / punctaSize);
    idx++;
  }
}

double
ZAssignPuncta::punctaTreeDist(const ZPunctum& punctum, const ZSwc* tree, ZSwc::ConstSwcTreeNode& nearestNode) const
{
  nearestNode = ZSwc::ConstSwcTreeNode();
  double min_dist = std::numeric_limits<double>::max();
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_imgInfo.voxelSizeXInUm(), m_imgInfo.voxelSizeYInUm(), m_imgInfo.voxelSizeZInUm());
  pt *= res;
  for (auto tn = tree->begin(); tn != tree->end(); ++tn) {
    if (!ZSwc::isRoot(tn) && tn->type != m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      double dist = glm::length(node - pt);
      if (dist > 100) {
        continue;
      }
      dist = pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), tn, ZSwc::parent(tn));
      if (dist < m_maxDistToBranch + punctum.radius() * m_imgInfo.voxelSizeXInUm() && dist < min_dist) { // in mask area
        min_dist = dist;
        nearestNode = tn;
      }
    }
  }
  return min_dist;
}

std::vector<ZSwc::ConstSwcTreeNode> ZAssignPuncta::nodesNearbyPuncta(const ZPunctum& punctum, const ZSwc* tree) const
{
  std::vector<ZSwc::ConstSwcTreeNode> nodes;
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_imgInfo.voxelSizeXInUm(), m_imgInfo.voxelSizeYInUm(), m_imgInfo.voxelSizeZInUm());
  pt *= res;
  for (auto tn = tree->begin(); tn != tree->end(); ++tn) {
    if (!ZSwc::isRoot(tn) && tn->type != m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      double dist = glm::length(node - pt);
      if (dist > 100) {
        continue;
      }
      dist = pointFrustumConeDist(punctum.x(), punctum.y(), punctum.z(), tn, ZSwc::parent(tn));
      if (dist < m_maxDistToBranch + punctum.radius() * m_imgInfo.voxelSizeXInUm()) { // in mask area
        nodes.push_back(tn);
      }
    }
  }
  return nodes;
}

double ZAssignPuncta::punctaSomaDist(const ZPunctum& punctum, const ZSwc* tree) const
{
  double min_dist = std::numeric_limits<double>::max();
  glm::dvec3 pt(punctum.x(), punctum.y(), punctum.z());
  glm::dvec3 res(m_imgInfo.voxelSizeXInUm(), m_imgInfo.voxelSizeYInUm(), m_imgInfo.voxelSizeZInUm());
  pt *= res;
  for (auto tn = tree->begin(); tn != tree->end(); ++tn) {
    if (ZSwc::isRoot(tn) || tn->type == m_somaType) {
      glm::dvec3 node(tn->x, tn->y, tn->z);
      node *= res;
      // typical soma diameter would be 10-15um
      if (double dist = glm::length(node - pt); dist < 20 && dist < min_dist) {
        min_dist = dist;
      }
    }
  }
  return min_dist;
}

double ZAssignPuncta::pointFrustumConeDist(double x,
                                           double y,
                                           double z,
                                           const ZSwc::ConstSwcTreeNode& tn,
                                           const ZSwc::ConstSwcTreeNode& ptn) const
{
  double dist;
  glm::dvec3 pt(x, y, z);
  glm::dvec3 bot(tn->x, tn->y, tn->z);
  glm::dvec3 top(ptn->x, ptn->y, ptn->z);
  glm::dvec3 res(m_imgInfo.voxelSizeXInUm(), m_imgInfo.voxelSizeYInUm(), m_imgInfo.voxelSizeZInUm());
  pt *= res;
  bot *= res;
  top *= res;
  double normtb = glm::dot(top - bot, top - bot);
  double normbp = glm::dot(bot - pt, bot - pt);
  double normtp = glm::dot(top - pt, top - pt);
  double dotbptb = glm::dot(bot - pt, top - bot);
  if (double frac = -dotbptb / normtb; frac < 0) {
    dist = std::sqrt(normbp) - tn->radius * m_imgInfo.voxelSizeXInUm();
  } else if (frac > 1) {
    dist = std::sqrt(normtp) - ptn->radius * m_imgInfo.voxelSizeXInUm();
  } else {
    double radius = m_imgInfo.voxelSizeXInUm() * ((1 - frac) * tn->radius + frac * ptn->radius);
    dist = std::sqrt(normbp - dotbptb * dotbptb / normtb) - radius;
  }
  return dist;
}

ZSwc::ConstSwcTreeNode ZAssignPuncta::intensityWeightedNearestNode(double x,
                                                                   double y,
                                                                   double z,
                                                                   const std::vector<ZSwc::ConstSwcTreeNode>& nodes,
                                                                   bool& isAmbiguous)
{
  // first crop out the region
  auto left = roundTo<int>(x);
  auto right = roundTo<int>(x);
  auto up = roundTo<int>(y);
  auto down = roundTo<int>(y);
  auto zup = roundTo<int>(z);
  auto zdown = roundTo<int>(z);
  for (auto node : nodes) {
    auto parent = ZSwc::parent(node);
    left = std::min(std::min(left, roundTo<int>(node->x)), roundTo<int>(parent->x));
    right = std::max(std::max(right, roundTo<int>(node->x)), roundTo<int>(parent->x));
    up = std::min(std::min(up, roundTo<int>(node->y)), roundTo<int>(parent->y));
    down = std::max(std::max(down, roundTo<int>(node->y)), roundTo<int>(parent->y));
    zup = std::min(std::min(zup, roundTo<int>(node->z)), roundTo<int>(parent->z));
    zdown = std::max(std::max(zdown, roundTo<int>(node->z)), roundTo<int>(parent->z));
  }

  left = std::max(0, left);
  right = std::min(right, static_cast<int>(m_imgInfo.width) - 1);
  up = std::max(0, up);
  down = std::min(down, static_cast<int>(m_imgInfo.height) - 1);
  zup = std::max(0, zup);
  zdown = std::min(zdown, static_cast<int>(m_imgInfo.depth) - 1);
  ZImgRegion rgn(left, right + 1, up, down + 1, zup, zdown + 1, m_dendriteChannel, m_dendriteChannel + 1, m_t, m_t + 1);
  ZImg img;
  if (m_img) {
    img = m_img->crop(rgn);
  } else {
    img = ZImg(m_filename, rgn, m_scene);
    if (!img.isType<uint8_t>()) {
      img = img.convertTo<uint8_t>(m_minValue, m_maxValue);
    }
  }

  ZImgGraph imgGraph(img);
  imgGraph.setConnectivity(26);
  ZImgAutoThreshold<> imgAutoThre;
  double cent1 = 0;
  double cent2 = 0;
  auto thre1 = imgAutoThre.centroidThre<double>(cent1, cent2, img);
  double scale = cent2 - cent1;
  if (scale < 1.0) {
    scale = 1.0;
  }
  scale /= 9.2;
  imgGraph.build(ZImgGraph::EdgeWeight2(thre1, scale));

  ZVoxelCoordinate startCoord(roundTo<int>(x) - left, roundTo<int>(y) - up, roundTo<int>(z) - zup);
  std::vector<double> dist = imgGraph.shortestPaths(startCoord);

  std::vector<double> nodeMinDists(nodes.size(), std::numeric_limits<double>::max());
  for (size_t v = 0; v < dist.size(); ++v) {
    ZVoxelCoordinate coord = img.indexToCoord(v);
    auto nodeIdx = nodes.size();
    for (auto i = nodes.size(); i-- > 0;) {
      if (pointFrustumConeDist(coord.x + left, coord.y + up, coord.z + zup, nodes[i], ZSwc::parent(nodes[i])) <= 0.0) {
        nodeIdx = i;
        break;
      }
    }
    if (nodeIdx < nodes.size()) {
      nodeMinDists[nodeIdx] = std::min(nodeMinDists[nodeIdx], dist[v]);
    }
  }

  auto minIndex = std::min_element(nodeMinDists.begin(), nodeMinDists.end()) - nodeMinDists.begin();
  auto minDist = nodeMinDists[minIndex];
  nodeMinDists[minIndex] = std::numeric_limits<double>::max();
  auto secondMinDist = *std::min_element(nodeMinDists.begin(), nodeMinDists.end());
  // LOG(INFO) << " min dist " << minDist;
  // LOG(INFO) << " second min dist " << secondMinDist;
  isAmbiguous = secondMinDist < m_ambiguousFactor * minDist;

  return nodes[minIndex];
}

ZSwc::ConstSwcTreeNode
ZAssignPuncta::nearestNode(double x, double y, double z, const std::vector<ZSwc::ConstSwcTreeNode>& nodes)
{
  double dist = std::numeric_limits<double>::max();
  ZSwc::ConstSwcTreeNode res;
  for (auto node : nodes) {
    if (double nodeDist = pointFrustumConeDist(x, y, z, node, ZSwc::parent(node)); nodeDist < dist) {
      dist = nodeDist;
      res = node;
    }
  }
  CHECK(!ZSwc::isNull(res));
  return res;
}

} // namespace nim
