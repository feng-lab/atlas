#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/line_func.glslinc"

void main()
{
  vec4 c; float d;
  fragment_func(c, d);
  FragData0 = c;
  gl_FragDepth = d;
}

