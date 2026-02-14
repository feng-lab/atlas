#version 450

// Specialization constants to select attribute set
layout(constant_id = 40) const bool USE_MESH_COLOR     = true;
layout(constant_id = 41) const bool USE_MESH_1DTEXTURE = false;
layout(constant_id = 42) const bool USE_MESH_2DTEXTURE = false;
layout(constant_id = 43) const bool USE_MESH_3DTEXTURE = false;

// Attributes
layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec3 attr_normal;
layout(location = 2) in vec4 attr_color;     // when USE_MESH_COLOR
layout(location = 3) in float attr_tc1d;     // when USE_MESH_1DTEXTURE
layout(location = 4) in vec2  attr_tc2d;     // when USE_MESH_2DTEXTURE
layout(location = 5) in vec3  attr_tc3d;     // when USE_MESH_3DTEXTURE

// Varyings
layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_tc1d;
layout(location = 2) out vec2  v_tc2d;
layout(location = 3) out vec3  v_tc3d;
layout(location = 4) out vec3  v_normal;
layout(location = 5) out vec3  v_point;

// UBOs for transforms/material
#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"

void main()
{
  vec3 normalWorld = xo.pos_transform_normal_matrix * attr_normal;
  v_normal = normalize(compute_view_normal_matrix() * normalWorld);
  vec4 vertex = xo.pos_transform * vec4(attr_vertex, 1.0);
  atlas_write_clip_distances(vertex);
  vec4 eyeSpacePos = xf.view_matrix * vertex;
  v_point = eyeSpacePos.xyz / eyeSpacePos.w;
  gl_Position = xf.projection_view_matrix * vertex;

  if (USE_MESH_COLOR)      v_color = attr_color;
  if (USE_MESH_1DTEXTURE)  v_tc1d  = attr_tc1d;
  if (USE_MESH_2DTEXTURE)  v_tc2d  = attr_tc2d;
  if (USE_MESH_3DTEXTURE)  v_tc3d  = attr_tc3d;
}
