#version 450
#extension GL_GOOGLE_include_directive : require

#define ATLAS_CLIP_DISTANCE_EXTRA_USE_DISCARD 0
#define ATLAS_PPLL 1
#include "include/matrices_material.glslinc"
#include "include/ellipsoid_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  if (atlas_should_reject_extra_clip_planes()) {
    return;
  }
  float fragDepth;
  if (!ellipsoid_fragment_depth_only(fragDepth)) {
    return;
  }
  gl_FragDepth = fragDepth;
  ppllCountFragment();
}
