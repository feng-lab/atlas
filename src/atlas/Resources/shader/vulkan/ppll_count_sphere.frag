#version 450
#extension GL_GOOGLE_include_directive : require

#define ATLAS_PPLL 1
#include "include/sphere_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  float fragDepth;
  if (!sphere_fragment_depth_only(fragDepth)) {
    return;
  }
  gl_FragDepth = fragDepth;
  ppllCountFragment();
}
