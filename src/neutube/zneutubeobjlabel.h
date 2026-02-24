#pragma once

#include "zimg.h"

#include <cstddef>

namespace nim::neutube {

struct LabelLargeObjectsParams
{
  // Foreground voxels are those whose value equals `flag` (matches legacy neuTube behavior).
  int flag = 1;

  // Voxels belonging to objects smaller than `minSize` are labeled as `smallLabel`.
  // (Legacy: `label` argument to Stack_Label_Large_Objects_*.)
  int smallLabel = 2;

  // Object size threshold. Objects with voxel count >= minSize are considered "large" and get labels starting at
  // smallLabel + 1, increasing for each large object.
  //
  // Legacy uses a signed int; if minSize <= 0 then all objects are considered large.
  int minSize = 1;

  // Neighborhood connectivity: 4, 8, 6, 10, 18, 26.
  int connectivity = 26;

  // Maximum label value before wrapping back to smallLabel + 1.
  // Legacy Default_Objlabel_Workspace() uses 65535.
  int maxLabel = 65535;

  // If true, increment the label for each large object encountered.
  bool incrementLargeLabel = true;
};

struct LabelLargeObjectsResult
{
  // Label image with the same dimensions as the input (single channel/time).
  // Background voxels are 0. Small objects are smallLabel. Large objects are labeled starting at smallLabel + 1.
  ZImg labels;

  // Number of large objects (objects with size >= minSize).
  size_t numLargeObjects = 0;
};

// C++ port of `Stack_Label_Large_Objects_*` object labeling semantics.
//
// Notes:
// - Only supports single-channel, single-time images (matches current CLI skeletonize usage).
// - Returns a label image whose voxel type matches legacy promotion behavior:
//   - Starts as uint8 when input is uint8 and labels fit.
//   - Promotes to uint16 at the moment a would-be large-object label exceeds 255, even if that particular object ends
//     up being re-labeled to `smallLabel` due to size (matches legacy translation trigger).
// - This function does not mutate the input image.
[[nodiscard]] LabelLargeObjectsResult labelLargeObjectsLegacy(const ZImg& img, const LabelLargeObjectsParams& params);

} // namespace nim::neutube
