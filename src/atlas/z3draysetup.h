#pragma once

#include "zglmutils.h"

#include <array>
#include <cstdint>

namespace nim {

inline constexpr uint32_t kZ3DAnalyticRaySetupMaxClipPlanes = 6u;

struct Z3DAnalyticRaySetup
{
  bool enabled = false;
  glm::mat4 ndcToTex{1.0f};
  glm::mat4 ndcToEye{1.0f};
  glm::vec3 boxMinTex{0.0f};
  uint32_t clipPlaneCount = 0u;
  glm::vec3 boxMaxTex{1.0f};
  uint32_t _reserved0 = 0u;
  glm::vec2 ndcZRange{-1.0f, 1.0f};
  glm::vec2 _reserved1{0.0f, 0.0f};
  std::array<glm::vec4, kZ3DAnalyticRaySetupMaxClipPlanes> clipPlanes{};
};

} // namespace nim
