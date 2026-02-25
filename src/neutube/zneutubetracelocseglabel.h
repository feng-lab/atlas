#pragma once

#include "zneutubelocalneuroseg.h"

#include "zimg.h"

#include <array>
#include <memory>

namespace nim::neutube {

class LocsegChain;

// C++ port of tz_workspace.h::Locseg_Label_Workspace, restricted to fields used by tracing.
struct LocsegLabelWorkspaceLegacyLike
{
  const ZImg* signal = nullptr;
  int option = 1;
  int flag = -1;
  int value = 0;
  double sratio = 1.0;
  double sdiff = 0.0;
  double slimit = 3.0;

  // Buffer mask used by LOCSEG_LABEL_OPTION_ADD / SUB (legacy options 6/7).
  std::unique_ptr<ZImg> bufferMask;

  // Updated by Local_Neuroseg_Label_W (legacy semantics). Range is
  // [sx, sy, sz, ex, ey, ez] and may use legacy clamping behavior.
  std::array<int, 6> range = {-1, -1, -1, -1, -1, -1};
};

// Port of tz_local_neuroseg.c::Local_Neuroseg_Label_G().
//
// - `flag >= 0`: only overwrite voxels whose current value equals `flag`.
// - `flag == -1`: no overwrite constraint.
// - `value` is written into `stack` for all voxels where the segment filter is positive.
void localNeurosegLabelGLegacyLike(const LocalNeuroseg& seg, ZImg& stack, int flag, int value, double zScale);

// Port of tz_local_neuroseg.c::Local_Neuroseg_Label_W().
void localNeurosegLabelWLegacyLike(const LocalNeuroseg& seg,
                                   ZImg& stack,
                                   double zScale,
                                   LocsegLabelWorkspaceLegacyLike& ws);

// Port of tz_locseg_chain.c::Locseg_Chain_Label_W().
void locsegChainLabelWLegacyLike(const LocsegChain& chain,
                                 ZImg& stack,
                                 double zScale,
                                 int begin,
                                 int end,
                                 LocsegLabelWorkspaceLegacyLike& ws);

} // namespace nim::neutube
