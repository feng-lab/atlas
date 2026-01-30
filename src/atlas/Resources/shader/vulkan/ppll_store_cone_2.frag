#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/matrices_material.glslinc"
#include "include/cone_func_2.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  vec4 color;
  float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;
  ppllStoreFragment(color, fragDepth);
}
