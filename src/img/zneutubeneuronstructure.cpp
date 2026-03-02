#include "zneutubeneuronstructure.h"

#include "zneutubelocalneuroseg.h"
#include "zneutubelocsegchain.h"
#include "zneutubelocsegchainknot.h"
#include "zneutubetraceconnectiontest.h"

#include "zswcops.h"

#include "zlog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace nim {

namespace {

[[nodiscard]] std::vector<Geo3dCircle>
locsegChainToCirclesScaledLegacyLike(const LocsegChain& chain, double xyScale, double zScale)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_To_Neuron_Component_S(..., GEO3D_CIRCLE).
  const auto kaOpt = locsegChainToKnotArrayLegacyLike(chain);
  if (!kaOpt) {
    return {};
  }

  const LocsegChainKnotArrayLegacyLike& ka = *kaOpt;
  std::vector<Geo3dCircle> circles;
  circles.reserve(ka.knots.size());

  int index = 0;
  int knotIndex = 0;

  for (const auto& node : chain) {
    LocalNeuroseg locseg2 = node.locseg;
    localNeurosegScaleLegacyLike(locseg2, xyScale, zScale);

    const LocsegChainKnotLegacyLike* knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
    while (knot != nullptr) {
      if (knot->id == index) {
        circles.push_back(localNeurosegToCircleTLegacyLike(locseg2, knot->offset, /*option*/ 0));
        ++knotIndex;
        knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
      } else {
        break;
      }
    }

    ++index;
  }

  CHECK(knotIndex == static_cast<int>(ka.knots.size()));
  return circles;
}

[[nodiscard]] int
closestCircleLegacyLike(const std::vector<Geo3dCircle>& circles, int start, int count, const std::array<double, 3>& pt)
{
  // Port of tz_neuron_structure.c::closest_circle(..., ort=NULL).
  CHECK(start >= 0);
  CHECK(count >= 1);
  CHECK(static_cast<size_t>(start + count) <= circles.size());

  int minIndex = 0;
  double minDist = std::numeric_limits<double>::infinity();

  for (int i = 0; i < count; ++i) {
    const auto& c = circles[static_cast<size_t>(start + i)];
    const double dx = c.center[0] - pt[0];
    const double dy = c.center[1] - pt[1];
    const double dz = c.center[2] - pt[2];
    const double dist = dx * dx + dy * dy + dz * dz;
    if (dist < minDist) {
      minDist = dist;
      minIndex = i;
    }
  }

  return minIndex;
}

} // namespace

NeuronStructureChainsLegacyLike locsegChainCompNeurostructLegacyLike(std::vector<std::unique_ptr<LocsegChain>>& chains,
                                                                     const ZImg* signal,
                                                                     double zScale,
                                                                     const ConnectionTestWorkspaceLegacyLike& ctw)
{
  NeuronStructureChainsLegacyLike ns;
  ns.chains.reserve(chains.size());
  for (auto& p : chains) {
    ns.chains.push_back(p.get());
  }

  const int n = static_cast<int>(ns.chains.size());
  ns.graph.nvertex = n;

  if (n == 0) {
    return ns;
  }

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      if (i == j) {
        continue;
      }

      NeurocompConnLegacyLike conn;
      defaultNeurocompConnLegacyLike(conn);
      conn.mode = NeurocompConnModeLegacyLike::HookLoop;

      LocsegChain* chainI = ns.chains[static_cast<size_t>(i)];
      LocsegChain* chainJ = ns.chains[static_cast<size_t>(j)];
      CHECK(chainI != nullptr);
      CHECK(chainJ != nullptr);

      if (!locsegChainConnectionTestLegacyLike(*chainI, *chainJ, signal, zScale, conn, ctw)) {
        continue;
      }

      neurocompConnTranslateModeLegacyLike(chainJ->length(), conn);

      bool connExisted = false;
      if (i > j) {
        const int edgeIdx = graphEdgeIndexLegacyLike(ns.graph, j, i);
        if (edgeIdx >= 0) {
          if (conn.mode == NeurocompConnModeLegacyLike::Link) {
            if (ns.conn[static_cast<size_t>(edgeIdx)].info[0] == conn.info[1]) {
              connExisted = true;
            }
          } else if (ns.conn[static_cast<size_t>(edgeIdx)].mode == NeurocompConnModeLegacyLike::Link) {
            if (ns.conn[static_cast<size_t>(edgeIdx)].info[1] == conn.info[0]) {
              connExisted = true;
            }
          }

          if (connExisted) {
            if (ns.conn[static_cast<size_t>(edgeIdx)].cost > conn.cost) {
              ns.conn[static_cast<size_t>(edgeIdx)] = conn;
              ns.graph.edges[static_cast<size_t>(edgeIdx)] = {i, j};
            }
          }
        }
      }

      if (!connExisted) {
        ns.graph.edges.push_back({i, j});
        ns.conn.push_back(conn);
      }
    }
  }

  return ns;
}

void processNeuronStructureLegacyLike(NeuronStructureChainsLegacyLike& ns)
{
  // Port of tz_neuron_structure.c::Process_Neuron_Structure().
  CHECK(ns.graph.edges.size() == ns.conn.size());

  const int nedge = static_cast<int>(ns.graph.edges.size());
  std::vector<std::uint8_t> edgeMask(static_cast<size_t>(nedge), 0u);

  for (int i = 0; i < nedge; ++i) {
    const auto mode = ns.conn[static_cast<size_t>(i)].mode;
    if (mode == NeurocompConnModeLegacyLike::Link) {
      edgeMask[static_cast<size_t>(i)] = 2u;
    } else if (mode == NeurocompConnModeLegacyLike::HookLoop) {
      edgeMask[static_cast<size_t>(i)] = 1u;
    } else {
      edgeMask[static_cast<size_t>(i)] = 0u;
    }
  }

  // Remove duplicated hook-loops by keeping smaller cost.
  for (int i = 0; i < nedge; ++i) {
    if (edgeMask[static_cast<size_t>(i)] != 1u) {
      continue;
    }
    for (int j = i + 1; j < nedge; ++j) {
      if (edgeMask[static_cast<size_t>(j)] != 1u) {
        continue;
      }
      if (ns.graph.edges[static_cast<size_t>(i)][0] == ns.graph.edges[static_cast<size_t>(j)][1] &&
          ns.graph.edges[static_cast<size_t>(i)][1] == ns.graph.edges[static_cast<size_t>(j)][0]) {
        if (ns.conn[static_cast<size_t>(i)].cost < ns.conn[static_cast<size_t>(j)].cost) {
          edgeMask[static_cast<size_t>(j)] = 0u;
        } else {
          edgeMask[static_cast<size_t>(i)] = 0u;
        }
      }
    }
  }

  // Keep the smallest edge for multiple-loop connections.
  for (int i = 0; i < nedge; ++i) {
    if (edgeMask[static_cast<size_t>(i)] != 1u) {
      continue;
    }
    for (int j = i + 1; j < nedge; ++j) {
      if (edgeMask[static_cast<size_t>(j)] != 1u) {
        continue;
      }
      if (ns.graph.edges[static_cast<size_t>(i)][0] == ns.graph.edges[static_cast<size_t>(j)][0] &&
          ns.conn[static_cast<size_t>(i)].info[0] == ns.conn[static_cast<size_t>(j)].info[0]) {
        if (ns.conn[static_cast<size_t>(i)].cost < ns.conn[static_cast<size_t>(j)].cost) {
          edgeMask[static_cast<size_t>(j)] = 0u;
        } else {
          edgeMask[static_cast<size_t>(i)] = 0u;
        }
      }
    }
  }

  // Compact edges/conn.
  int out = 0;
  for (int i = 0; i < nedge; ++i) {
    if (edgeMask[static_cast<size_t>(i)] > 0u) {
      if (i != out) {
        ns.graph.edges[static_cast<size_t>(out)] = ns.graph.edges[static_cast<size_t>(i)];
        ns.conn[static_cast<size_t>(out)] = ns.conn[static_cast<size_t>(i)];
      }
      ++out;
    }
  }

  ns.graph.edges.resize(static_cast<size_t>(out));
  ns.conn.resize(static_cast<size_t>(out));
}

NeuronStructureCirclesLegacyLike
neuronStructureLocsegChainToCircleSLegacyLike(const NeuronStructureChainsLegacyLike& ns, double xyScale, double zScale)
{
  // Port of tz_neuron_structure.c::Neuron_Structure_Locseg_Chain_To_Circle_S().
  NeuronStructureCirclesLegacyLike out;
  out.graph.nvertex = 0;

  const int nchain = static_cast<int>(ns.chains.size());
  if (nchain == 0) {
    return out;
  }

  std::vector<int> startId(static_cast<size_t>(nchain + 1), 0);
  startId[0] = 0;

  for (int i = 0; i < nchain; ++i) {
    const LocsegChain* chain = ns.chains[static_cast<size_t>(i)];
    CHECK(chain != nullptr);

    const std::vector<Geo3dCircle> circles = locsegChainToCirclesScaledLegacyLike(*chain, xyScale, zScale);
    const int n = static_cast<int>(circles.size());

    const int base = startId[static_cast<size_t>(i)];
    out.circles.insert(out.circles.end(), circles.begin(), circles.end());

    // Internal link edges with cost 0.
    for (int k = 1; k < n; ++k) {
      out.graph.edges.push_back({base + k - 1, base + k});
      out.connCost.push_back(0.0);
    }

    startId[static_cast<size_t>(i + 1)] = base + n;
  }

  out.graph.nvertex = static_cast<int>(out.circles.size());

  // External edges translated from chain-level connections.
  const int nedge = static_cast<int>(ns.graph.edges.size());
  CHECK(nedge == static_cast<int>(ns.conn.size()));

  for (int i = 0; i < nedge; ++i) {
    const int chainId0 = ns.graph.edges[static_cast<size_t>(i)][0];
    const int chainId1 = ns.graph.edges[static_cast<size_t>(i)][1];
    CHECK(chainId0 >= 0 && chainId0 < nchain);
    CHECK(chainId1 >= 0 && chainId1 < nchain);

    const NeurocompConnLegacyLike& conn2 = ns.conn[static_cast<size_t>(i)];
    if (conn2.mode != NeurocompConnModeLegacyLike::Link && conn2.mode != NeurocompConnModeLegacyLike::HookLoop) {
      continue;
    }

    int id0 = 0;
    if (conn2.info[0] == 0) {
      id0 = startId[static_cast<size_t>(chainId0)];
    } else {
      id0 = startId[static_cast<size_t>(chainId0 + 1)] - 1;
    }

    const std::array<double, 3> pos = {conn2.pos[0] * xyScale, conn2.pos[1] * xyScale, conn2.pos[2] * zScale};

    const int start = startId[static_cast<size_t>(chainId1)];
    const int count = startId[static_cast<size_t>(chainId1 + 1)] - start;
    if (count <= 0) {
      continue;
    }

    const int local = closestCircleLegacyLike(out.circles, start, count, pos);
    const int id1 = start + local;

    out.graph.edges.push_back({id0, id1});
    out.connCost.push_back(conn2.cost);
  }

  CHECK(out.graph.edges.size() == out.connCost.size());
  return out;
}

void neuronStructureToTreeLegacyLike(NeuronStructureCirclesLegacyLike& ns)
{
  // Port of tz_neuron_structure.c::Neuron_Structure_To_Tree().
  CHECK(ns.graph.edges.size() == ns.connCost.size());

  const std::vector<double> oldConn = ns.connCost;

  ns.graph.weights = oldConn;
  const GraphMst2ResultLegacyLike mst = graphToMst2LegacyLike(ns.graph);

  std::vector<double> newConn;
  newConn.reserve(ns.graph.edges.size());
  for (size_t i = 0; i < mst.edgeIn.size(); ++i) {
    if (mst.edgeIn[i] == 1u) {
      newConn.push_back(oldConn[i]);
    }
  }
  ns.connCost = std::move(newConn);

  CHECK(ns.graph.edges.size() == ns.connCost.size());
  CHECK(ns.graph.weights.size() == ns.graph.edges.size());
}

std::unique_ptr<ZSwc> neuronStructureToSwcTreeCircleZLegacyLike(const NeuronStructureCirclesLegacyLike& ns,
                                                                double zScale)
{
  // Port of tz_neuron_structure.c::Neuron_Structure_To_Swc_Tree_Circle_Z(..., root_pos=NULL).
  const int ncomp = ns.graph.nvertex;
  if (ncomp <= 0) {
    return nullptr;
  }

  CHECK(static_cast<int>(ns.circles.size()) == ncomp);

  auto tree = std::make_unique<ZSwc>();

  std::vector<ZSwc::SwcTreeNode> tnArray;
  tnArray.resize(static_cast<size_t>(ncomp));

  for (int i = 0; i < ncomp; ++i) {
    const Geo3dCircle& circle = ns.circles[static_cast<size_t>(i)];

    SwcNode swcNode;
    // Match tz_neuron_structure.c::Neuron_Structure_To_Swc_Tree_Circle_Z():
    // nodes are created as regular nodes (id=1) and only later (after the connection
    // pass) d==0 nodes are marked virtual and regularized.
    swcNode.id = 1;
    swcNode.type = 2;
    swcNode.x = circle.center[0];
    swcNode.y = circle.center[1];
    swcNode.z = circle.center[2];
    if (zScale != 1.0) {
      swcNode.z /= zScale;
    }
    swcNode.radius = circle.radius;
    swcNode.parentID = -1;

    tnArray[static_cast<size_t>(i)] = tree->appendRoot(swcNode);
  }

  const int nconn = static_cast<int>(ns.graph.edges.size());
  CHECK(nconn == static_cast<int>(ns.connCost.size()));

  for (int i = 0; i < nconn; ++i) {
    const int parent = ns.graph.edges[static_cast<size_t>(i)][0];
    const int child = ns.graph.edges[static_cast<size_t>(i)][1];
    CHECK(parent >= 0 && parent < ncomp);
    CHECK(child >= 0 && child < ncomp);

    swcTreeNodeSetRootLegacyLike(*tree, tnArray[static_cast<size_t>(child)]);
    tree->appendChild(tnArray[static_cast<size_t>(parent)], tnArray[static_cast<size_t>(child)]);
    tnArray[static_cast<size_t>(child)]->weight = ns.connCost[static_cast<size_t>(i)];
  }

  // Legacy marks nodes with d==0 as virtual and then calls Swc_Tree_Regularize().
  for (auto it = tree->begin(); it != tree->end(); ++it) {
    if (it->radius == 0.0) {
      it->id = -1;
    }
  }

  swcTreeRegularizeLegacyLike(*tree);

  return tree;
}

} // namespace nim
