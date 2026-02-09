#version 450
#extension GL_GOOGLE_include_directive : require

layout(early_fragment_tests) in;

#include "include/mesh_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  vec4 color;
  float fragDepth;
  fragment_func(color, fragDepth);

  ppllStoreFragment(color, fragDepth);
}
