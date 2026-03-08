#pragma once

#include "zneutubelocalneuroseg.h"

#include "zswc.h"

#include <optional>

namespace nim {

// Port of `Local_Neuroseg_Change_Top`.
void localNeurosegChangeTopLegacyLike(LocalNeuroseg& locseg, const std::array<double, 3>& newTop);

// Port of `Swc_Tree_Node_To_Locseg` (restricted to the regular-node case).
//
// SWC coordinates are image-space coordinates. The returned locseg is converted
// into trace space so it can be used by the legacy tracing/mask code.
[[nodiscard]] std::optional<LocalNeuroseg> swcNodeToLocsegLegacyLike(const ZSwc::ConstSwcTreeNode& node,
                                                                     double zToXYRatio);

} // namespace nim
