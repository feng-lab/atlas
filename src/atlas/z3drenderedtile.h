#pragma once

#include "zimg.h"

#include <optional>

namespace nim {

// Host-owned, guard-free output from one complete worker-engine spatial tile
// attempt. Images are RGBA and top-left-origin regardless of Vulkan readback
// orientation. primaryColor is mono or left-eye output; rightColor is present
// only when the same attempt rendered the complete stereo pair.
struct Z3DRenderedTile
{
  ZImg primaryColor;
  std::optional<ZImg> rightColor;
};

} // namespace nim
