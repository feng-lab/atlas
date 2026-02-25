#pragma once

#include "zneutubegeo3dcircle.h"
#include "zneutubegraph.h"
#include "zneutubeneurocompconn.h"
#include "zneutubetraceconnectiontestworkspace.h"

#include "zimg.h"
#include "zswc.h"

#include <memory>
#include <vector>

namespace nim::neutube {

class LocsegChain;

// C++ port of `tz_neuron_structure.h::Neuron_Structure` for the subset used by
// auto-trace reconstruction.
struct NeuronStructureChainsLegacyLike
{
  GraphLegacyLike graph; // vertices are chains; edges are oriented pairs (id1,id2)
  std::vector<NeurocompConnLegacyLike> conn; // same indexing as graph.edges
  std::vector<LocsegChain*> chains; // non-owning (may be modified by interpolation)
};

// Builds a chain-level neuron structure (ports `Locseg_Chain_Comp_Neurostruct`).
//
// Notes:
// - `signal` is optional; when null, signal-dependent connection tests (shortest-path test)
//   are skipped. Auto-trace passes the real signal image.
[[nodiscard]] NeuronStructureChainsLegacyLike
locsegChainCompNeurostructLegacyLike(std::vector<std::unique_ptr<LocsegChain>>& chains,
                                     /*nullable*/ const ZImg* signal,
                                     double zScale,
                                     const ConnectionTestWorkspaceLegacyLike& ctw);

// Port of `Process_Neuron_Structure` (removes duplicate/low-priority hook-loop edges).
void processNeuronStructureLegacyLike(NeuronStructureChainsLegacyLike& ns);

// Circle-level neuron structure used for SWC conversion.
struct NeuronStructureCirclesLegacyLike
{
  GraphLegacyLike graph; // vertices are circles; edges are oriented pairs (parent, child)
  std::vector<double> connCost; // same indexing as graph.edges
  std::vector<Geo3dCircle> circles; // component array (vertex-indexed)
};

// Port of `Neuron_Structure_Locseg_Chain_To_Circle_S`.
[[nodiscard]] NeuronStructureCirclesLegacyLike
neuronStructureLocsegChainToCircleSLegacyLike(const NeuronStructureChainsLegacyLike& ns, double xyScale, double zScale);

// Port of `Neuron_Structure_To_Tree` (MST over weighted graph).
void neuronStructureToTreeLegacyLike(NeuronStructureCirclesLegacyLike& ns);

// Port of `Neuron_Structure_To_Swc_Tree_Circle_Z`.
[[nodiscard]] std::unique_ptr<ZSwc>
neuronStructureToSwcTreeCircleZLegacyLike(const NeuronStructureCirclesLegacyLike& ns, double zScale);

} // namespace nim::neutube
