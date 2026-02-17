#pragma once

#include "zglmutils.h"

#include <array>

namespace nim {

// Vulkan clip planes are used to implement the same local/global XYZ cut
// features as the OpenGL backend. Local cuts (object-space AABB) and global
// cuts (world-space AABB) are both applied, so the combined region can require
// up to 12 planes (6 local + 6 global).
//
// Device limits are validated at Vulkan device creation time; see ZVulkanContext.
inline constexpr size_t kVulkanMaxClipPlanes = 12;
// We only export up to this many planes via gl_ClipDistance (fixed-function
// early clipping). Any remaining planes are applied via fragment discard using
// interpolated distances.
inline constexpr size_t kVulkanMaxClipDistances = 8;
static_assert(kVulkanMaxClipDistances <= kVulkanMaxClipPlanes);
inline constexpr size_t kVulkanMaxExtraClipPlanes = kVulkanMaxClipPlanes - kVulkanMaxClipDistances;

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

// Per-frame transform UBO (std140). This carries only the state that is expected
// to vary when the camera moves (view/projection matrices, ortho flag, etc).
//
// Keep this file in lock-step with:
//   - Resources/shader/vulkan/include/matrices_material.glslinc
struct FrameTransformsUBOStd140
{
  glm::mat4 projection_view_matrix{1.0f};
  glm::mat4 view_matrix{1.0f};
  glm::mat4 projection_matrix{1.0f};
  glm::mat4 inverse_projection_matrix{1.0f};
  // parameters.y = ortho flag (0 perspective, 1 ortho)
  // parameters.z = sphere boxCorrection (used by sphere shaders; 0 for most pipelines)
  glm::vec4 parameters{0.0f, 0.0f, 0.0f, 0.0f};
};

// Per-object transform UBO (std140). This carries state that is expected to
// remain stable during camera-only motion (model transform, local/global clip
// planes, etc).
//
// Keep this file in lock-step with:
//   - Resources/shader/vulkan/include/matrices_material.glslinc
struct ObjectTransformsUBOStd140
{
  glm::mat4 pos_transform{1.0f};
  // Model-space normal matrix (inverse-transpose of the model matrix's upper 3x3).
  // View-space normal transform is applied in the shader using the frame view matrix.
  std::array<glm::vec4, 3> pos_transform_normal_matrix{glm::vec4(1.0f, 0.0f, 0.0f, 0.0f),
                                                       glm::vec4(0.0f, 1.0f, 0.0f, 0.0f),
                                                       glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)};
  // parameters.x = sizeScale (global)
  glm::vec4 parameters{1.0f, 0.0f, 0.0f, 0.0f};
  // x = enabled (0/1), y = planeCount, z/w reserved
  glm::ivec4 clip_params{0, 0, 0, 0};
  std::array<glm::vec4, kVulkanMaxClipPlanes> clip_planes{};
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
