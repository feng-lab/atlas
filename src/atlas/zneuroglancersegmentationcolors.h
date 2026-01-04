#pragma once

#include "zimginterface.h"

#include <cstdint>

namespace nim::neuroglancer {

// SplitMix64 constants from Steele et al.; used for fast, deterministic hashing.
[[nodiscard]] inline uint64_t splitmix64(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Deterministically assigns a pseudo-random color to a segmentation label ID.
// ID 0 is reserved for "empty/background" and mapped to black.
[[nodiscard]] inline col4 labelColorForId(uint64_t id, uint8_t alpha = 255_u8)
{
  if (id == 0) {
    return col4{0, 0, 0, alpha};
  }

  const uint64_t h = splitmix64(id);

  // Avoid very dark colors by biasing RGB up.
  constexpr uint8_t kMinChannel = 64;
  constexpr uint8_t kChannelSpan = 191; // 64..255 inclusive
  const uint8_t r = static_cast<uint8_t>(kMinChannel + ((h >> 0) & 0xff) * kChannelSpan / 255);
  const uint8_t g = static_cast<uint8_t>(kMinChannel + ((h >> 8) & 0xff) * kChannelSpan / 255);
  const uint8_t b = static_cast<uint8_t>(kMinChannel + ((h >> 16) & 0xff) * kChannelSpan / 255);
  return col4{r, g, b, alpha};
}

} // namespace nim::neuroglancer

