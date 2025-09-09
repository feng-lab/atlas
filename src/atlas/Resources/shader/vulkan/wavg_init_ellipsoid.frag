#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/matrices_material.glslinc"
#include "include/ellipsoid_func.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;
  FragData0 = color;
  FragData1.xy = vec2(1.0, fragDepth);
}

