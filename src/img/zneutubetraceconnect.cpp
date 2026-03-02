#include "zneutubetraceconnect.h"

#include "zneutubeimgsampling.h"
#include "zswctreenodegeomlegacy.h"

#include "zimg.h"
#include "zlog.h"
#include "zvoxelvolume.h"

#include <cmath>
#include <limits>
#include <vector>

namespace nim {

namespace {

struct SwcNodeConnectionLegacyLike
{
  ZSwc::SwcTreeNode hook;
  ZSwc::SwcTreeNode loop;
  double dist = std::numeric_limits<double>::infinity();
};

[[nodiscard]] bool isRegularNodeLegacyLike(const ZSwc::ConstSwcTreeNode& n)
{
  return !ZSwc::isNull(n) && n->id >= 0;
}

[[nodiscard]] double
swcConnectorDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b, bool surfaceDist)
{
  // Port of ZSwcConnector::computeDistance().
  CHECK(isRegularNodeLegacyLike(a));
  CHECK(isRegularNodeLegacyLike(b));

  double d = surfaceDist ? swcNodeSurfaceDistanceLegacyLike(a, b) : swcNodeDistanceLegacyLike(a, b);

  if (d < 0.0) {
    const double r1 = a->radius;
    const double r2 = b->radius;
    if (r1 == 0.0 || r2 == 0.0) {
      d = 0.0;
    } else {
      d = swcNodeDistanceLegacyLike(a, b) / r1 / r2;
    }
  }

  return d;
}

[[nodiscard]] SwcNodeConnectionLegacyLike identifyConnectionLegacyLike(const ZSwc::SwcTreeNode& head,
                                                                       const ZSwc::SwcTreeNode& tail,
                                                                       ZSwc& swc,
                                                                       const std::vector<ZSwc::SwcTreeNode>& loopRoots,
                                                                       double minDist,
                                                                       bool surfaceDist)
{
  // Port of:
  // - ZSwcConnector::identifyConnection(const ZSwcPath&, const ZSwcTree&)
  // - ZSwcConnector::identifyConnection(const ZSwcPath&, const vector<ZSwcTree*>&)
  SwcNodeConnectionLegacyLike best;
  if (ZSwc::isNull(head) || loopRoots.empty()) {
    return best;
  }

  best.dist = std::numeric_limits<double>::infinity();

  for (const auto& root : loopRoots) {
    if (ZSwc::isNull(root)) {
      continue;
    }

    for (auto it = swc.begin(root); it != swc.end(root); ++it) {
      if (!isRegularNodeLegacyLike(it)) {
        continue;
      }

      const double dHead = swcConnectorDistanceLegacyLike(head, it, surfaceDist);
      if (dHead < best.dist) {
        best.dist = dHead;
        best.hook = head;
        best.loop = it;
      }

      if (!ZSwc::isNull(tail)) {
        const double dTail = swcConnectorDistanceLegacyLike(tail, it, surfaceDist);
        if (dTail < best.dist) {
          best.dist = dTail;
          best.hook = tail;
          best.loop = it;
        }
      }
    }
  }

  // Legacy threshold is strict: connection is only accepted when dist < minDist.
  if (!(best.dist < minDist)) {
    best = SwcNodeConnectionLegacyLike{};
  }

  return best;
}

} // namespace

template<typename StackLike>
[[nodiscard]] double findBestTerminalBreakLegacyLikeImpl(const std::array<double, 3>& terminalCenter,
                                                         double terminalRadius,
                                                         const std::array<double, 3>& innerCenter,
                                                         double innerRadius,
                                                         const StackLike& stack)
{
  // Port of ZNeuronTracer::findBestTerminalBreak().
  const double dx = terminalCenter[0] - innerCenter[0];
  const double dy = terminalCenter[1] - innerCenter[1];
  const double dz = terminalCenter[2] - innerCenter[2];
  const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d < 0.5) {
    return 1.0;
  }

  std::array<double, 3> dvec = {dx, dy, dz};
  const double norm = std::sqrt(dvec[0] * dvec[0] + dvec[1] * dvec[1] + dvec[2] * dvec[2]);
  if (norm > 0.0) {
    dvec[0] /= norm;
    dvec[1] /= norm;
    dvec[2] /= norm;
  }

  const double innerIntensity = pointSampleLegacyLike(stack, innerCenter[0], innerCenter[1], innerCenter[2]);
  if (innerIntensity == 0.0) {
    return 1.0;
  }

  double lambda = 1.0;
  for (lambda = 1.0; lambda >= 0.3; lambda -= 0.1) {
    const double radius = terminalRadius * lambda + innerRadius * (1.0 - lambda);
    const std::array<double, 3> currentEnd = {innerCenter[0] + dvec[0] * (d * lambda + radius),
                                              innerCenter[1] + dvec[1] * (d * lambda + radius),
                                              innerCenter[2] + dvec[2] * (d * lambda + radius)};

    const double terminalIntensity = pointSampleLegacyLike(stack, currentEnd[0], currentEnd[1], currentEnd[2]);
    if (terminalIntensity / innerIntensity > 0.3) {
      break;
    }
  }

  return lambda;
}

double findBestTerminalBreakLegacyLike(const std::array<double, 3>& terminalCenter,
                                       double terminalRadius,
                                       const std::array<double, 3>& innerCenter,
                                       double innerRadius,
                                       const ZImg& stack)
{
  return findBestTerminalBreakLegacyLikeImpl(terminalCenter, terminalRadius, innerCenter, innerRadius, stack);
}

double findBestTerminalBreakLegacyLike(const std::array<double, 3>& terminalCenter,
                                       double terminalRadius,
                                       const std::array<double, 3>& innerCenter,
                                       double innerRadius,
                                       const ZVoxelVolume& stack)
{
  return findBestTerminalBreakLegacyLikeImpl(terminalCenter, terminalRadius, innerCenter, innerRadius, stack);
}

template<typename StackLike>
void connectBranchToHostLegacyLikeImpl(ZSwc& swc,
                                       const std::vector<ZSwc::SwcTreeNode>& hostRoots,
                                       ZSwc::SwcTreeNode branchRoot,
                                       const StackLike& stack)
{
  // Port of ZNeuronTracer::connectBranch(const ZSwcPath&, ZSwcTree*).
  if (ZSwc::isNull(branchRoot)) {
    return;
  }

  if (ZSwc::isNull(ZSwc::firstChild(branchRoot))) {
    return;
  }

  auto leafTail = [](ZSwc::SwcTreeNode start) -> ZSwc::SwcTreeNode {
    ZSwc::SwcTreeNode n = start;
    while (true) {
      const auto child = ZSwc::firstChild(n);
      if (ZSwc::isNull(child)) {
        break;
      }
      n = child;
    }
    return n;
  };

  const auto tail = leafTail(branchRoot);

  constexpr double MinConnDistLegacyLike = 10.0;
  const SwcNodeConnectionLegacyLike conn =
    identifyConnectionLegacyLike(branchRoot, tail, swc, hostRoots, MinConnDistLegacyLike, /*surfaceDist*/ true);

  ZSwc::SwcTreeNode hook = conn.hook;
  ZSwc::SwcTreeNode loop = conn.loop;

  if (!ZSwc::isNull(hook)) {
    bool needAdjust = false;

    if (!ZSwc::isRoot(hook)) {
      swc.setAsRoot(hook);
      branchRoot = hook;
    }

    if (swcNodesHasOverlapLegacyLike(hook, loop)) {
      needAdjust = true;
    } else {
      const auto hookChild = ZSwc::firstChild(hook);
      if (!ZSwc::isNull(hookChild) && swcNodesFormingTurnLegacyLike(loop, hook, hookChild)) {
        needAdjust = true;
      }
    }

    if (needAdjust) {
      const auto child = ZSwc::firstChild(branchRoot);
      if (!ZSwc::isNull(child)) {
        swcNodeAverageLegacyLike(branchRoot, child, branchRoot);
        const auto hookChild = ZSwc::firstChild(hook);
        if (!ZSwc::isNull(hookChild) && swcNodesFormingTurnLegacyLike(loop, hook, hookChild)) {
          swcNodeAverageLegacyLike(child, loop, branchRoot);
        }
      }
    }
  } else {
    if (const auto rootNeighbor = ZSwc::firstChild(branchRoot); !ZSwc::isNull(rootNeighbor)) {
      const std::array<double, 3> rootCenter = {branchRoot->x, branchRoot->y, branchRoot->z};
      const std::array<double, 3> nbrCenter = {rootNeighbor->x, rootNeighbor->y, rootNeighbor->z};

      const double lambda =
        findBestTerminalBreakLegacyLikeImpl(rootCenter, branchRoot->radius, nbrCenter, rootNeighbor->radius, stack);

      if (lambda < 1.0) {
        swcNodeInterpolateLegacyLike(branchRoot, rootNeighbor, lambda, branchRoot);
      }
    }
  }

  if (!ZSwc::isNull(hook)) {
    auto neighbors = swcNeighborArrayLegacyLike(swc, loop);
    for (auto tn : neighbors) {
      if (swcNodesHasSignificantOverlapLegacyLike(tn, hook)) {
        loop = tn;
        const auto newHook = ZSwc::firstChild(hook);
        if (!ZSwc::isNull(newHook)) {
          swc.erase(hook);
          hook = newHook;
          branchRoot = hook;
        }
      }
    }
  }

  if (const auto child = ZSwc::firstChild(branchRoot); !ZSwc::isNull(child)) {
    const auto terminal = leafTail(branchRoot);
    const auto terminalNeighbor = ZSwc::parent(terminal);
    if (!ZSwc::isNull(terminalNeighbor)) {
      const std::array<double, 3> terminalCenter = {terminal->x, terminal->y, terminal->z};
      const std::array<double, 3> nbrCenter = {terminalNeighbor->x, terminalNeighbor->y, terminalNeighbor->z};

      const double lambda = findBestTerminalBreakLegacyLikeImpl(terminalCenter,
                                                                terminal->radius,
                                                                nbrCenter,
                                                                terminalNeighbor->radius,
                                                                stack);
      if (lambda < 1.0) {
        swcNodeInterpolateLegacyLike(terminal, terminalNeighbor, lambda, terminal);
      }
    }
  }

  if (!ZSwc::isNull(hook)) {
    CHECK(!ZSwc::isNull(loop));
    swc.appendChild(loop, hook);
  }
}

void connectBranchToHostLegacyLike(ZSwc& swc,
                                   const std::vector<ZSwc::SwcTreeNode>& hostRoots,
                                   ZSwc::SwcTreeNode branchRoot,
                                   const ZImg& stack)
{
  connectBranchToHostLegacyLikeImpl(swc, hostRoots, branchRoot, stack);
}

void connectBranchToHostLegacyLike(ZSwc& swc,
                                   const std::vector<ZSwc::SwcTreeNode>& hostRoots,
                                   ZSwc::SwcTreeNode branchRoot,
                                   const ZVoxelVolume& stack)
{
  connectBranchToHostLegacyLikeImpl(swc, hostRoots, branchRoot, stack);
}

} // namespace nim
