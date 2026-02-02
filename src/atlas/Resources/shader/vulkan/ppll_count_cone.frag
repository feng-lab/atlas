#version 450
#extension GL_GOOGLE_include_directive : require

#define ATLAS_PPLL 1
#include "include/matrices_material.glslinc"
#include "include/cone_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  float fragDepth;
  if (!cone_fragment_depth_only(fragDepth)) {
    return;
  }
  gl_FragDepth = fragDepth;
  ppllCountFragment();
}
