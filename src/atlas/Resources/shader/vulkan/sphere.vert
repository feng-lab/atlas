#version 450

// Attributes
layout(location = 0) in vec4 attr_vertex; // xyz center, w radius
layout(location = 1) in vec4 attr_color;
layout(location = 2) in float attr_flags;
layout(location = 3) in vec4 attr_specular_shininess; // xyz specular, w shininess

// Varyings
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_sphere_center;
layout(location = 2) out float v_radius2;
layout(location = 3) out vec3 v_point;
layout(location = 4) out vec4 v_material_specular; // .w packs shininess when dynamic

#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"

layout(constant_id = 60) const bool USE_DYNAMIC_MATERIAL = false;

void main()
{
  float radius = attr_vertex.w * xo.parameters.x;

  vec2 flags = mod(floor(vec2(attr_flags / 16.0, attr_flags)), 16.0);
  float rightFlag = flags.x - 1.0; // -1 or 1
  float upFlag    = flags.y - 1.0; // -1 or 1

  vec4 centerVertex = xo.pos_transform * vec4(attr_vertex.xyz, 1.0);
  atlas_write_clip_distances(centerVertex);
  v_color = attr_color;
  v_radius2 = radius * radius;
  if (USE_DYNAMIC_MATERIAL) {
    // Pack shininess into the .w component to avoid an extra varying
    v_material_specular = attr_specular_shininess;
  } else {
    v_material_specular = vec4(0.0);
  }

  vec3 rightVector = vec3(xf.view_matrix[0][0], xf.view_matrix[1][0], xf.view_matrix[2][0]);
  vec3 upVector    = vec3(xf.view_matrix[0][1], xf.view_matrix[1][1], xf.view_matrix[2][1]);
  vec3 cornerDirection = (xf.parameters.z * upFlag) * upVector + (xf.parameters.z * rightFlag) * rightVector;

  vec4 vertex = vec4(centerVertex.xyz + radius * cornerDirection, 1.0);
  vec4 eyeSpacePos = xf.view_matrix * vertex;
  v_point = eyeSpacePos.xyz;

  vec4 tmppos = xf.view_matrix * centerVertex;
  v_sphere_center = tmppos.xyz;

  gl_Position = xf.projection_view_matrix * vertex;
}
