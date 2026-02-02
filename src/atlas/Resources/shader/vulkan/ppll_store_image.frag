#version 450
#extension GL_GOOGLE_include_directive : require

#define ATLAS_PPLL 1
#include "include/copyimage_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  vec4 color;
  float fragDepth;
  if (!fragment_func(color, fragDepth)) {
    return;
  }
  gl_FragDepth = fragDepth;
  ppllStoreFragment(color, fragDepth);
}
