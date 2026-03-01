#pragma once

#include "zimg.h"
#include "zswc.h"

namespace nim {

// Port of neuTube's SWC node signal fitting used by:
// - ZSwcSignalFitter::fitSignal(...)
// - ZStackDoc::executeSwcNodeEstimateRadiusCommand()
//
// This adjusts an SWC node's (x,y,radius) based on the image content at z=round(node.z).
//
// Notes:
// - This is a strict port of `SwcTreeNode::fitSignal(...)` in neuTube.
// - The input `stack` must be a single-channel, single-time ZImg volume (numChannels==1, numTimes==1).
enum class ZNeutubeImageBackgroundLegacyLike
{
  Dark,
  Bright,
};

// Port of `SwcTreeNode::fitSignal(tn, stack, bg, option)` (option 1=RC, 2=Triangle).
// Returns true when the node was updated.
[[nodiscard]] bool
fitSwcNodeSignalLegacyLike(SwcNode& node, const ZImg& stack, ZNeutubeImageBackgroundLegacyLike bg, int option);

// Same as fitSwcNodeSignalLegacyLike(), but operates on the already-cropped slice used by the legacy code:
// `slice = crop(stack, x1, y1, cz, x2-x1+1, y2-y1+1, 1)`.
//
// - `slice` must be depth==1, numChannels==1, numTimes==1.
// - `x0,y0` are the global coordinates of slice pixel (0,0).
//
// This exists so callers that cannot materialize a full in-memory `stack` (e.g. paged datasets) can still
// run the exact legacy computation using a cropped read.
[[nodiscard]] bool fitSwcNodeSignalInCroppedSliceLegacyLike(SwcNode& node,
                                                            const ZImg& slice,
                                                            int x0,
                                                            int y0,
                                                            ZNeutubeImageBackgroundLegacyLike bg,
                                                            int option);

// Port of `ZSwcSignalFitter::fitSignal(...)` behavior: try option=1 (RC), then fallback to option=2 (Triangle).
[[nodiscard]] bool
fitSwcNodeSignalWithFallbackLegacyLike(SwcNode& node, const ZImg& stack, ZNeutubeImageBackgroundLegacyLike bg);

[[nodiscard]] bool fitSwcNodeSignalWithFallbackInCroppedSliceLegacyLike(SwcNode& node,
                                                                        const ZImg& slice,
                                                                        int x0,
                                                                        int y0,
                                                                        ZNeutubeImageBackgroundLegacyLike bg);

} // namespace nim
