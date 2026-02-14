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
  vec4 color;
  float fragDepth;
  if (!fragment_func(color, fragDepth)) {
    return;
  }
  gl_FragDepth = fragDepth;
  ppllStoreFragment(color, fragDepth);
}
