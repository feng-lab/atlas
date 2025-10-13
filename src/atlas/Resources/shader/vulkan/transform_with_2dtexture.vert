#version 450

layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec2 attr_2dTexCoord0;

layout(location = 0) out vec2 texCoord0;

layout(push_constant) uniform Transform2D
{
  mat4 projection_view_matrix;
} xf;

void main()
{
  gl_Position = xf.projection_view_matrix * vec4(attr_vertex, 1.0);
  texCoord0 = attr_2dTexCoord0;
}
