#pragma once

#include "zneutubelocalneuroseg.h"

#include "zswc.h"

#include <optional>

namespace nim {

// Port of `Local_Neuroseg_Change_Top`.
void localNeurosegChangeTopLegacyLike(LocalNeuroseg& locseg, const std::array<double, 3>& newTop);

// Port of `Swc_Tree_Node_To_Locseg` (restricted to the regular-node case).
[[nodiscard]] std::optional<LocalNeuroseg> swcNodeToLocsegLegacyLike(const ZSwc::ConstSwcTreeNode& node);

} // namespace nim
