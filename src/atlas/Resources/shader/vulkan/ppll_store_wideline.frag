#version 450
#extension GL_GOOGLE_include_directive : require

layout(early_fragment_tests) in;

#define ATLAS_PPLL 1
#include "include/wideline_func1.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  vec4 color;
  float fragDepth;
  if (!fragment_func(color, fragDepth)) {
    return;
  }
  ppllStoreFragment(color, fragDepth);
}
