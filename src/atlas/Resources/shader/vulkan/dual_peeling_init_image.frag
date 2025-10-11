#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/copyimage_func.glslinc"

void main()
{
  vec4 c; float fragDepth;
  fragment_func(c, fragDepth);
  gl_FragDepth = fragDepth;
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}

