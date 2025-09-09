#version 450

layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec2 attr_2dTexCoord0;

layout(location = 0) out vec2 texCoord0;

void main()
{
  gl_Position = vec4(attr_vertex, 1.0);
  texCoord0 = attr_2dTexCoord0;
}

