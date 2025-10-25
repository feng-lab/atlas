#pragma once

#include "zglmutils.h"

#include <array>

namespace nim {

// Shared std140 UBO layouts mirrored by Vulkan mesh/line pipelines. Keep the
// packing rules identical to the GLSL includes under Resources/shader/vulkan.
struct alignas(16) LightingUBOStd140
{
  struct alignas(16) LightSource
  {
    glm::vec4 position{0.0f};
    glm::vec4 ambient{0.0f};
    glm::vec4 diffuse{0.0f};
    glm::vec4 specular{0.0f};
    glm::vec3 attenuation{1.0f, 0.0f, 0.0f};
    float _pad0 = 0.0f;
    float spotCutoff = 180.0f;
    float spotExponent = 1.0f;
    glm::vec2 _pad1{0.0f};
    glm::vec3 spotDirection{0.0f, 0.0f, -1.0f};
    float _pad2 = 0.0f;
  };

  alignas(4) int lighting_enabled = 0;
  alignas(4) int numLights = 0;
  alignas(8) glm::vec2 _padHeader{0.0f};
  // Global ambient term for the scene
  alignas(16) glm::vec4 scene_ambient{0.0f};
  alignas(16) glm::vec3 fog_color_top{0.0f};
  alignas(4) float fog_end = 0.0f;
  alignas(16) glm::vec3 fog_color_bottom{0.0f};
  alignas(4) float fog_scale = 0.0f;
  alignas(8) float fog_density_log2e = 0.0f;
  alignas(8) float fog_density_density_log2e = 0.0f;
  alignas(8) glm::vec2 screen_dim_RCP{0.0f};
  alignas(4) float weighted_blended_depth_scale = 1.0f;
  alignas(4) float _pad2 = 0.0f;
  alignas(4) float ze_to_zw_a = 0.0f;
  alignas(4) float ze_to_zw_b = 0.0f;
  alignas(8) glm::vec2 _pad3{0.0f};
  std::array<LightSource, 5> lights{};
};

static_assert(alignof(LightingUBOStd140) == 16, "Lighting UBO must be 16-byte aligned (std140)");
static_assert(alignof(LightingUBOStd140::LightSource) == 16,
              "Lighting light source must be 16-byte aligned (std140 element stride)");

struct TransformsUBOStd140
{
  glm::mat4 projection_view_matrix{1.0f};
  glm::mat4 view_matrix{1.0f};
  glm::mat4 pos_transform{1.0f};
  std::array<glm::vec4, 3> pos_transform_normal_matrix{glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
                                                       glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
                                                       glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)};
  glm::mat4 projection_matrix{1.0f};
  glm::mat4 inverse_projection_matrix{1.0f};
  glm::vec4 parameters{1.0f, 0.0f, 0.0f, 0.0f};
};

struct MaterialUBOStd140
{
  glm::vec4 material_ambient{1.0f};
  glm::vec4 material_specular{0.0f};
  float material_shininess = 32.0f;
  float alpha = 1.0f;
  int use_custom_color = 0;
  float _pad0 = 0.0f;
  glm::vec4 custom_color{1.0f};
};

} // namespace nim
