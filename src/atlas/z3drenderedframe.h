#pragma once

#include "z3drenderedtile.h"
#include "zglmutils.h"

#include <optional>

namespace nim {

class Z3DTileDescriptor;

// Save-oriented, top-left-origin RGBA output assembled from one complete tile
// batch. rightColor is present only for a stereo-pair batch.
struct Z3DRenderedFrame
{
  Z3DRenderedFrame(glm::uvec2 fullOutputExtent, bool stereoPair);

  void pasteTile(const Z3DTileDescriptor& descriptor, const Z3DRenderedTile& renderedTile);

  ZImg primaryColor;
  std::optional<ZImg> rightColor;
};

} // namespace nim
