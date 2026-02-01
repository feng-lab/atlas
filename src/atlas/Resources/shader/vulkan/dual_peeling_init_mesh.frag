#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/mesh_func.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}
