#version 450

layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec3 attr_3dTexCoord0;

layout(location = 0) out vec3 texCoord0;
layout(location = 1) out vec4 eyeCoord;

#include "include/matrices_material.glslinc"

void main()
{
  gl_Position = xf.projection_view_matrix * vec4(attr_vertex, 1.0);
  texCoord0 = attr_3dTexCoord0;
  eyeCoord = xf.view_matrix * vec4(attr_vertex, 1.0);
}

