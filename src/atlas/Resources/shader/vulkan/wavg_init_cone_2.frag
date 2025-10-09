#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/matrices_material.glslinc"
#include "include/cone_func_2.glslinc"

void main()
{
  vec4 color;
  float depth;
  fragment_func(color, depth);
  gl_FragDepth = depth;
  FragData0 = color;
  FragData1.xy = vec2(1.0, depth);
}
