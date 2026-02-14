#version 450
#extension GL_GOOGLE_include_directive : require

layout(early_fragment_tests) in;

#define ATLAS_CLIP_DISTANCE_EXTRA_USE_DISCARD 0
#include "include/mesh_func.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  if (atlas_should_reject_extra_clip_planes()) {
    return;
  }
  vec4 color;
  float fragDepth;
  fragment_func(color, fragDepth);

  ppllStoreFragment(color, fragDepth);
}
