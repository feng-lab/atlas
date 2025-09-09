#version 450

layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec4 attr_color;

layout(location = 0) out vec4 v_color;

#include "include/matrices_material.glslinc"

void main()
{
  vec4 vertex = xf.pos_transform * vec4(attr_vertex, 1.0);
  gl_Position = xf.projection_view_matrix * vertex;
  v_color = attr_color;
}

