#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec4 attr_color;

layout(location = 0) out vec4 colorIn;

#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"

void main()
{
  vec4 vertex = xo.pos_transform * vec4(attr_vertex, 1.0);
  atlas_write_clip_distances(vertex);
  gl_Position = xf.projection_view_matrix * vertex;
  colorIn = attr_color;
}
