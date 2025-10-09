#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/matrices_material.glslinc"
#include "include/cone_func_2.glslinc"

void main()
{
  vec4 color;
  float depth;
  fragment_func(color, depth);
  FragData0 = color;
  gl_FragDepth = depth;
}
