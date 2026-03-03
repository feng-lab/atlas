#include "zneutubecompareswc.h"

#include "zswcops.h"

#include "zexception.h"
#include "zlog.h"
#include "zswc.h"
#include "zneutubemathutils.h"

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nim {

namespace {

struct Point3d
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

[[nodiscard]] double dist3d(double x0, double y0, double z0, double x1, double y1, double z1)
{
  const double dx = x0 - x1;
  const double dy = y0 - y1;
  const double dz = z0 - z1;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] double nodeDist(const SwcNode& a, const SwcNode& b)
{
  return dist3d(a.x, a.y, a.z, b.x, b.y, b.z);
}

void rescaleSwc(ZSwc& tree, double scaleX, double scaleY, double scaleZ, bool changingRadius)
{
  const double dScale = std::sqrt(scaleX * scaleY);
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    it->x *= scaleX;
    it->y *= scaleY;
    it->z *= scaleZ;
    if (changingRadius) {
      it->radius *= dScale;
    }
  }
}

void setNodeLabel(ZSwc& tree, int64_t label)
{
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    it->label = label;
  }
}

[[nodiscard]] double treeLengthLegacy(const ZSwc& tree)
{
  double length = 0.0;
  for (auto it = tree.cbegin(); it != tree.cend(); ++it) {
    const auto parent = ZSwc::parent(it);
    if (ZSwc::isNull(parent)) {
      continue;
    }
    length += nodeDist(*it, *parent);
  }
  return length;
}

struct SwcPath
{
  ZSwc* host = nullptr;
  std::vector<ZSwc::SwcTreeNode> nodes;

  [[nodiscard]] bool empty() const
  {
    return nodes.empty();
  }

  [[nodiscard]] size_t size() const
  {
    return nodes.size();
  }

  [[nodiscard]] ZSwc::SwcTreeNode& operator[](size_t i)
  {
    return nodes[i];
  }

  [[nodiscard]] const ZSwc::SwcTreeNode& operator[](size_t i) const
  {
    return nodes[i];
  }

  [[nodiscard]] ZSwc::SwcTreeNode& back()
  {
    return nodes.back();
  }

  [[nodiscard]] const ZSwc::SwcTreeNode& back() const
  {
    return nodes.back();
  }

  void reverse()
  {
    std::reverse(nodes.begin(), nodes.end());
  }

  void label(int64_t v)
  {
    for (auto& n : nodes) {
      n->label = v;
    }
  }
};

[[nodiscard]] std::vector<ZSwc::SwcTreeNode> zSortedNodesLegacy(ZSwc& tree)
{
  std::vector<ZSwc::SwcTreeNode> nodes;
  nodes.reserve(tree.size());

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    nodes.push_back(it);
  }

  std::sort(nodes.begin(), nodes.end(), [](const ZSwc::SwcTreeNode& a, const ZSwc::SwcTreeNode& b) {
    return a->z < b->z;
  });

  return nodes;
}

[[nodiscard]] SwcPath extractMainTrunkLayerLegacy(ZSwc& tree, double step)
{
  SwcPath path;
  path.host = &tree;

  if (tree.empty()) {
    return path;
  }

  const std::vector<ZSwc::SwcTreeNode> nodeArray = zSortedNodesLegacy(tree);
  if (nodeArray.empty()) {
    return path;
  }

  size_t nodeIndex = 0;
  double currentZ = nodeArray[nodeIndex]->z;

  while (nodeIndex < nodeArray.size()) {
    bool isValid = true;

    if (nodeIndex > 0) {
      if (std::fabs(nodeArray[nodeIndex]->z - currentZ) > std::fabs(nodeArray[nodeIndex - 1]->z - currentZ)) {
        isValid = false;
        currentZ += step;
      }
    }

    if (isValid) {
      if (nodeIndex + 1 < nodeArray.size()) {
        if (std::fabs(nodeArray[nodeIndex]->z - currentZ) >= std::fabs(nodeArray[nodeIndex + 1]->z - currentZ)) {
          isValid = false;
          ++nodeIndex;
        }
      }
    }

    if (isValid) {
      path.nodes.push_back(nodeArray[nodeIndex]);
      ++nodeIndex;
      currentZ += step;
    }
  }

  return path;
}

[[nodiscard]] std::array<double, 2>
layerShollFeatureLegacy(const ZSwc& tree, const ZSwc::SwcTreeNode& tn, double layerMargin)
{
  CHECK(!ZSwc::isNull(tn));

  const double baseZ = tn->z;
  const double upZ = baseZ - layerMargin;
  const double downZ = baseZ + layerMargin;

  double count = 0.0;

  // Legacy counts edges between each node and its parent. In the C/neurolabi model, regular roots
  // have a virtual parent at z=0; emulate this for roots (no parent in ZSwc).
  for (auto it = tree.cbeginBreadthFirst(); it != tree.cendBreadthFirst(); ++it) {
    const double z1 = it->z;
    double z2 = 0.0;
    const auto parent = ZSwc::parent(it);
    if (!ZSwc::isNull(parent)) {
      z2 = parent->z;
    }

    const double minZ = std::min(z1, z2);
    const double maxZ = std::max(z1, z2);
    if (minZ <= downZ && maxZ >= upZ) {
      count += 1.0;
    }
  }

  return {baseZ, count};
}

[[nodiscard]] double layerShollFeatureSimilarityLegacy(const std::array<double, 2>& f1,
                                                       const std::array<double, 2>& f2,
                                                       double layerBaseFactor,
                                                       double layerScale)
{
  const double layerDiff = std::fabs(f1[0] - f2[0]);

  double s1 = f1[1];
  double s2 = f2[1];
  if (s1 > s2) {
    std::swap(s1, s2);
  }

  CHECK(s1 > 0.0);

  return std::sqrt(s1) * s1 / s2 / (layerDiff / layerScale + layerBaseFactor);
}

struct LayerMatcherConfig
{
  double trunkStep = 200.0;
  double sampleStep = 200.0;
  int matchingLevel = 2;

  double layerBaseFactor = 1.0;
  double layerScale = 4000.0;
  double layerMargin = 100.0;
};

[[nodiscard]] std::array<double, 2>
computeNodeFeatureCachedLegacy(const ZSwc& tree, const LayerMatcherConfig& cfg, const ZSwc::SwcTreeNode& tn)
{
  CHECK(!ZSwc::isNull(tn));

  // Legacy `ZSwcNodeBufferFeatureAnalyzer` caches helper results into node weight/feature.
  if (tn->feature == 0.0) {
    const auto helper = layerShollFeatureLegacy(tree, tn, cfg.layerMargin);
    tn->weight = helper[0];
    tn->feature = helper[1];
  }

  return {tn->weight, tn->feature};
}

struct SimMatrix
{
  int rows = 0;
  int cols = 0;
  std::vector<double> data;

  SimMatrix(int r, int c)
    : rows(r)
    , cols(c)
    , data(static_cast<size_t>(r) * static_cast<size_t>(c), 0.0)
  {}

  [[nodiscard]] int size() const
  {
    return rows * cols;
  }

  [[nodiscard]] double at(int r, int c) const
  {
    return data[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
  }

  void set(int r, int c, double v)
  {
    data[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] = v;
  }

  [[nodiscard]] int sub2index(int r, int c) const
  {
    if (r < 0 || c < 0 || r >= rows || c >= cols) {
      return -1;
    }
    return r * cols + c;
  }

  [[nodiscard]] std::pair<int, int> index2sub(int index) const
  {
    CHECK(cols > 0);
    return {index / cols, index % cols};
  }
};

struct DpMatchResult
{
  double score = 0.0;
  std::map<int, int> matches;
};

[[nodiscard]] DpMatchResult dynamicProgrammingMatchLegacy(const SimMatrix& simMat, double gapPenalty)
{
  std::vector<double> matchingTable(static_cast<size_t>(simMat.rows + 1) * static_cast<size_t>(simMat.cols + 1), 0.0);

  auto tableAt = [&](int r, int c) -> double& {
    return matchingTable[static_cast<size_t>(r) * static_cast<size_t>(simMat.cols + 1) + static_cast<size_t>(c)];
  };
  auto tableValue = [&](int r, int c) -> double {
    return matchingTable[static_cast<size_t>(r) * static_cast<size_t>(simMat.cols + 1) + static_cast<size_t>(c)];
  };

  std::vector<int> parent(static_cast<size_t>(std::max(0, simMat.size())), -1);

  for (int i = 0; i < simMat.rows; ++i) {
    for (int j = 0; j < simMat.cols; ++j) {
      double score = tableValue(i, j) + simMat.at(i, j);
      double maxScore = score;
      parent[static_cast<size_t>(simMat.sub2index(i, j))] = simMat.sub2index(i - 1, j - 1);

      score = tableValue(i + 1, j) - gapPenalty;
      if (score > maxScore) {
        maxScore = score;
        parent[static_cast<size_t>(simMat.sub2index(i, j))] = simMat.sub2index(i, j - 1);
      }

      score = tableValue(i, j + 1) - gapPenalty;
      if (score > maxScore) {
        maxScore = score;
        parent[static_cast<size_t>(simMat.sub2index(i, j))] = simMat.sub2index(i - 1, j);
      }

      tableAt(i + 1, j + 1) = maxScore;
    }
  }

  int bestIndex = -1;
  double matchingScore = -gapPenalty * 10.0;

  const int lastColumnIndex = simMat.cols;
  const int lastRowIndex = simMat.rows;

  for (int i = 1; i < simMat.rows + 1; ++i) {
    const double v = tableValue(i, lastColumnIndex);
    if (v > matchingScore) {
      matchingScore = v;
      bestIndex = simMat.sub2index(i - 1, lastColumnIndex - 1);
    }
  }

  for (int j = 1; j < simMat.cols + 1; ++j) {
    const double v = tableValue(lastRowIndex, j);
    if (v > matchingScore) {
      matchingScore = v;
      bestIndex = simMat.sub2index(lastRowIndex - 1, j - 1);
    }
  }

  std::vector<int> trace;
  for (int idx = bestIndex; idx >= 0;) {
    trace.push_back(idx);
    idx = parent[static_cast<size_t>(idx)];
  }

  std::map<int, int> matches;
  for (int idx : trace) {
    const auto sub = simMat.index2sub(idx);
    matches[sub.first] = sub.second;
  }

  return {matchingScore, std::move(matches)};
}

[[nodiscard]] std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>>
matchGLegacy(const LayerMatcherConfig& cfg, SwcPath* branch1, SwcPath* branch2, int level, double* outScore)
{
  CHECK(branch1 != nullptr);
  CHECK(branch2 != nullptr);

  CHECK(branch1->host != nullptr || branch1->empty());
  CHECK(branch2->host != nullptr || branch2->empty());

  branch1->label(level);
  branch2->label(level);

  const int rows = static_cast<int>(branch1->size());
  const int cols = static_cast<int>(branch2->size());

  std::vector<std::array<double, 2>> feat1;
  std::vector<std::array<double, 2>> feat2;
  feat1.reserve(branch1->size());
  feat2.reserve(branch2->size());

  for (size_t i = 0; i < branch1->size(); ++i) {
    feat1.push_back(computeNodeFeatureCachedLegacy(*branch1->host, cfg, (*branch1)[i]));
  }
  for (size_t i = 0; i < branch2->size(); ++i) {
    feat2.push_back(computeNodeFeatureCachedLegacy(*branch2->host, cfg, (*branch2)[i]));
  }

  SimMatrix simMat(rows, cols);
  for (int i = 0; i < rows; ++i) {
    for (int j = 0; j < cols; ++j) {
      simMat.set(i,
                 j,
                 layerShollFeatureSimilarityLegacy(feat1[static_cast<size_t>(i)],
                                                   feat2[static_cast<size_t>(j)],
                                                   cfg.layerBaseFactor,
                                                   cfg.layerScale));
    }
  }

  const DpMatchResult dp = dynamicProgrammingMatchLegacy(simMat, 0.1);
  if (outScore != nullptr) {
    *outScore = dp.score;
  }

  std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> result;
  result.reserve(dp.matches.size());
  for (const auto& [r, c] : dp.matches) {
    if (r < 0 || c < 0) {
      continue;
    }
    result.emplace_back((*branch1)[static_cast<size_t>(r)], (*branch2)[static_cast<size_t>(c)]);
  }

  return result;
}

struct MatchingSource
{
  ZSwc::SwcTreeNode node1;
  ZSwc::SwcTreeNode exclude1;
  ZSwc::SwcTreeNode node2;
  ZSwc::SwcTreeNode exclude2;
};

void updateMatchingSourceLegacy(std::queue<MatchingSource>* sourceQueue,
                                ZSwc* tree1,
                                ZSwc* tree2,
                                const std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>>& matching)
{
  CHECK(sourceQueue != nullptr);
  CHECK(tree1 != nullptr);
  CHECK(tree2 != nullptr);

  for (const auto& m : matching) {
    MatchingSource source{};

    int n1 = 0;
    for (auto it = tree1->beginChild(m.first); it != tree1->endChild(m.first); ++it) {
      if (it->label > 0) {
        source.exclude1 = it;
      } else {
        ++n1;
      }
    }

    int n2 = 0;
    for (auto it = tree2->beginChild(m.second); it != tree2->endChild(m.second); ++it) {
      if (it->label > 0) {
        source.exclude2 = it;
      } else {
        ++n2;
      }
    }

    if (n1 > 0 && n2 > 0) {
      source.node1 = m.first;
      source.node2 = m.second;
      sourceQueue->push(source);
    }
  }
}

[[nodiscard]] std::vector<ZSwc::SwcTreeNode> gatherBranchNodesLegacy(const ZSwc::SwcTreeNode& begin,
                                                                     const ZSwc::SwcTreeNode& end)
{
  CHECK(!ZSwc::isNull(begin));
  CHECK(!ZSwc::isNull(end));

  std::vector<ZSwc::SwcTreeNode> nodes;

  auto cur = end;
  while (!ZSwc::isNull(cur)) {
    nodes.push_back(cur);
    if (cur == begin) {
      break;
    }
    cur = ZSwc::parent(cur);
  }

  CHECK(!nodes.empty());
  CHECK(nodes.back() == begin) << "End is not downstream of begin.";

  std::reverse(nodes.begin(), nodes.end());
  return nodes;
}

[[nodiscard]] double branchLengthLegacy(const std::vector<ZSwc::SwcTreeNode>& nodes)
{
  if (nodes.size() < 2) {
    return 0.0;
  }

  double len = 0.0;
  for (size_t i = 1; i < nodes.size(); ++i) {
    len += nodeDist(*nodes[i - 1], *nodes[i]);
  }
  return len;
}

[[nodiscard]] std::vector<Point3d> sampleBranchLegacy(const std::vector<ZSwc::SwcTreeNode>& nodes, double step)
{
  CHECK(!nodes.empty());

  std::vector<Point3d> sampleArray;
  sampleArray.push_back({nodes.front()->x, nodes.front()->y, nodes.front()->z});

  if (nodes.size() < 2) {
    return sampleArray;
  }

  double targetLength = step;
  double prevLength = 0.0;
  double curLength = 0.0;

  for (size_t k = 1; k < nodes.size(); ++k) {
    const auto& parent = nodes[k - 1];
    const auto& child = nodes[k];
    const double dist = nodeDist(*parent, *child);
    curLength += dist;

    while (targetLength <= curLength && dist > 0.0) {
      const double lambda = (targetLength - prevLength) / dist;
      const double x = (1.0 - lambda) * parent->x + lambda * child->x;
      const double y = (1.0 - lambda) * parent->y + lambda * child->y;
      const double z = (1.0 - lambda) * parent->z + lambda * child->z;
      sampleArray.push_back({x, y, z});
      targetLength += step;
    }

    prevLength = curLength;
  }

  return sampleArray;
}

void resampleBranchLegacy(ZSwc& tree, const ZSwc::SwcTreeNode& begin, const ZSwc::SwcTreeNode& end, double step)
{
  CHECK(!ZSwc::isNull(begin));
  CHECK(!ZSwc::isNull(end));

  std::vector<ZSwc::SwcTreeNode> nodes = gatherBranchNodesLegacy(begin, end);
  const double len = branchLengthLegacy(nodes);

  if (len <= 0.0) {
    return;
  }

  const int nseg = iroundLegacyLike(len / step);
  if (nseg > 1) {
    const double realStep = len / static_cast<double>(nseg);
    const std::vector<Point3d> pointArray = sampleBranchLegacy(nodes, realStep);

    const int desiredNodes = nseg + 1;
    const int currentNodes = static_cast<int>(nodes.size());

    if (desiredNodes > currentNodes) {
      const int n = desiredNodes - currentNodes;
      for (int i = 0; i < n; ++i) {
        (void)addBreak(tree, end, 0.5);
      }
    } else {
      const int n = currentNodes - desiredNodes;
      for (int i = 0; i < n; ++i) {
        const auto p = ZSwc::parent(end);
        CHECK(!ZSwc::isNull(p));
        mergeToParent(tree, p);
      }
    }

    auto tn = end;
    for (int i = nseg - 1; i > 0; --i) {
      tn = ZSwc::parent(tn);
      CHECK(!ZSwc::isNull(tn));
      tn->x = pointArray[static_cast<size_t>(i)].x;
      tn->y = pointArray[static_cast<size_t>(i)].y;
      tn->z = pointArray[static_cast<size_t>(i)].z;
    }
  } else {
    if (begin != end) {
      while (ZSwc::parent(end) != begin) {
        const auto p = ZSwc::parent(end);
        if (ZSwc::isNull(p)) {
          break;
        }
        mergeToParent(tree, p);
      }
    }
  }
}

[[nodiscard]] int labelBranchesLegacy(ZSwc& tree)
{
  int label = 0;

  for (auto rootIt = tree.beginRoot(); rootIt != tree.endRoot(); ++rootIt) {
    rootIt->label = ++label;
    bool first = true;
    for (auto it = tree.begin(rootIt); it != tree.end(rootIt); ++it) {
      if (first) {
        first = false;
        continue;
      }
      const auto parent = ZSwc::parent(it);
      CHECK(!ZSwc::isNull(parent));
      if (tree.numChildren(parent) > 1) {
        it->label = ++label;
      } else {
        it->label = parent->label;
      }
    }
  }

  return label;
}

[[nodiscard]] std::optional<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> extractBranchByLabelLegacy(ZSwc& tree,
                                                                                                        int label)
{
  std::vector<ZSwc::SwcTreeNode> nodes;
  nodes.reserve(tree.size());
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    nodes.push_back(it);
  }
  std::reverse(nodes.begin(), nodes.end());

  ZSwc::SwcTreeNode lowerEnd;
  ZSwc::SwcTreeNode upperEnd;

  for (const auto& tn : nodes) {
    if (tn->label == label) {
      if (ZSwc::isNull(lowerEnd)) {
        lowerEnd = tn;
      }
      upperEnd = tn;
    } else if (!ZSwc::isNull(lowerEnd)) {
      const auto p = ZSwc::parent(upperEnd);
      if (!ZSwc::isNull(p)) {
        upperEnd = p;
      }
      break;
    }
  }

  if (ZSwc::isNull(lowerEnd) || ZSwc::isNull(upperEnd)) {
    return std::nullopt;
  }

  return std::make_pair(upperEnd, lowerEnd);
}

void resampleSwcLegacy(ZSwc& tree, double step)
{
  const int n = labelBranchesLegacy(tree);
  for (int i = 1; i <= n; ++i) {
    const auto br = extractBranchByLabelLegacy(tree, i);
    if (!br) {
      continue;
    }
    resampleBranchLegacy(tree, br->first, br->second, step);
  }
}

[[nodiscard]] double compareSwcLegacyAlgorithm(const LayerMatcherConfig& cfg, const ZSwc& a, const ZSwc& b)
{
  if (a.empty() || b.empty()) {
    return 0.0;
  }

  ZSwc tree1ForMatch = a;
  ZSwc tree2ForMatch = b;

  resampleSwcLegacy(tree1ForMatch, cfg.sampleStep);
  resampleSwcLegacy(tree2ForMatch, cfg.sampleStep);

  if (tree1ForMatch.empty() || tree2ForMatch.empty()) {
    return 0.0;
  }

  std::queue<MatchingSource> sourceQueue;

  SwcPath branch1 = extractMainTrunkLayerLegacy(tree1ForMatch, cfg.trunkStep);
  SwcPath branch2 = extractMainTrunkLayerLegacy(tree2ForMatch, cfg.trunkStep);

  if (branch1.empty() || branch2.empty()) {
    return 0.0;
  }

  tree1ForMatch.setAsRoot(branch1[0]);

  const Point3d v1{branch1[0]->x - branch1.back()->x,
                   branch1[0]->y - branch1.back()->y,
                   branch1[0]->z - branch1.back()->z};
  const Point3d v2{branch2[0]->x - branch2.back()->x,
                   branch2[0]->y - branch2.back()->y,
                   branch2[0]->z - branch2.back()->z};
  const double dot = v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
  if (dot < 0.0) {
    branch2.reverse();
  }

  tree2ForMatch.setAsRoot(branch2[0]);

  setNodeLabel(tree1ForMatch, 0);
  setNodeLabel(tree2ForMatch, 0);

  double currentScore = 0.0;

  {
    double score = 0.0;
    auto matchingResult = matchGLegacy(cfg, &branch1, &branch2, 1, &score);
    currentScore = score;
    updateMatchingSourceLegacy(&sourceQueue, &tree1ForMatch, &tree2ForMatch, matchingResult);
  }

  while (!sourceQueue.empty()) {
    const MatchingSource source = sourceQueue.front();
    sourceQueue.pop();

    const int currentLevel = static_cast<int>(source.node1->label) + 1;
    if (currentLevel <= cfg.matchingLevel) {
      SwcPath empty1;
      empty1.host = &tree1ForMatch;
      SwcPath empty2;
      empty2.host = &tree2ForMatch;

      double score = 0.0;
      const auto matchingResult = matchGLegacy(cfg, &empty1, &empty2, currentLevel, &score);
      currentScore += score;
      updateMatchingSourceLegacy(&sourceQueue, &tree1ForMatch, &tree2ForMatch, matchingResult);
    }
  }

  double finalScore = currentScore;

  const double s1 = treeLengthLegacy(tree1ForMatch);
  const double s2 = treeLengthLegacy(tree2ForMatch);
  const double denom = (s1 < s2) ? std::sqrt(s2) : std::sqrt(s1);
  if (denom > 0.0) {
    finalScore /= denom;
  }
  finalScore *= 1000.0;

  return finalScore;
}

} // namespace

CompareSwcResult computeCompareSwc(const std::vector<std::string>& inputPaths, double scale)
{
  if (inputPaths.empty()) {
    throw std::invalid_argument("Compare SWC: please specify input SWC files.");
  }

  std::vector<ZSwc> trees;
  trees.reserve(inputPaths.size());

  for (const auto& p : inputPaths) {
    ZSwc tree;
    tree.load(QString::fromStdString(p));
    if (scale != 1.0) {
      rescaleSwc(tree, scale, scale, scale, true);
    }
    trees.push_back(std::move(tree));
  }

  LayerMatcherConfig cfg;

  std::vector<double> selfScore(trees.size(), 0.0);
  for (size_t i = 0; i < trees.size(); ++i) {
    selfScore[i] = compareSwcLegacyAlgorithm(cfg, trees[i], trees[i]);
  }

  CompareSwcResult result;
  result.inputs = inputPaths;

  for (size_t i = 0; i < trees.size(); ++i) {
    for (size_t j = i + 1; j < trees.size(); ++j) {
      double score = compareSwcLegacyAlgorithm(cfg, trees[i], trees[j]);
      const double denom = std::max(selfScore[i], selfScore[j]);
      if (denom != 0.0) {
        score /= denom;
      }
      result.pairs.push_back(CompareSwcPairScore{i, j, score});
    }
  }

  return result;
}

std::string formatCompareSwcPairs(const CompareSwcResult& result)
{
  std::ostringstream stream;
  for (const auto& p : result.pairs) {
    stream << p.i << "-" << p.j << ": " << p.score << std::endl;
  }
  return stream.str();
}

int runCompareSwc(const std::vector<std::string>& inputPaths, double scale)
{
  if (inputPaths.empty()) {
    LOG(ERROR) << "Compare SWC: please specify input SWC files.";
    return 1;
  }

  LOG(INFO) << "Computing pairwise similarity for:";
  for (const auto& p : inputPaths) {
    LOG(INFO) << "  " << p;
  }

  CompareSwcResult result;
  try {
    result = computeCompareSwc(inputPaths, scale);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Compare SWC failed: " << e.what();
    return 1;
  }

  LOG(INFO) << "Result:";
  for (size_t i = 0; i < result.inputs.size(); ++i) {
    LOG(INFO) << i << ": " << result.inputs[i];
  }
  LOG(INFO) << formatCompareSwcPairs(result);

  return 1;
}

} // namespace nim
