#version 450

layout(location = 0) in vec3 attr_vertex;

void main()
{
  gl_Position = vec4(attr_vertex, 1.0);
}

