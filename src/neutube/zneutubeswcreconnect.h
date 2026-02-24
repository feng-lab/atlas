#pragma once

#include "zswc.h"

namespace nim::neutube {

// Port of `Swc_Tree_Label_Forest`.
// Labels each connected component (rooted tree) in the forest with a unique positive integer label.
// Returns the number of roots/components.
int labelForest(ZSwc* tree);

// Port of `Swc_Tree_Reconnect` (connects forest roots using an MST over inter-tree distances).
void reconnectSwc(ZSwc* tree, double zScale, double distThre);

} // namespace nim::neutube
