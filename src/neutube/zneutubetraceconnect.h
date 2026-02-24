#pragma once

#include "zswc.h"

#include <array>
#include <vector>

namespace nim {

class ZImg;

}

namespace nim::neutube {

// Port of `ZNeuronTracer::findBestTerminalBreak`.
//
// Determines how far a terminal node should be retracted toward its neighbor based on intensity sampling.
// Returns `lambda` in [0.3, 1.0] in increments of 0.1 (or 1.0 if early-exit conditions are met).
[[nodiscard]] double findBestTerminalBreakLegacyLike(const std::array<double, 3>& terminalCenter,
                                                     double terminalRadius,
                                                     const std::array<double, 3>& innerCenter,
                                                     double innerRadius,
                                                     const ZImg& stack);

// Port of `ZNeuronTracer::connectBranch` (branch is a single chain rooted at `branchRoot`).
//
// `hostRoots` must contain only the host SWC roots (excluding the just-added traced branch root),
// to match legacy `identifyConnection(branch, host)` behavior.
void connectBranchToHostLegacyLike(ZSwc* swc,
                                   const std::vector<ZSwc::SwcTreeNode>& hostRoots,
                                   ZSwc::SwcTreeNode branchRoot,
                                   const ZImg& stack);

} // namespace nim::neutube
