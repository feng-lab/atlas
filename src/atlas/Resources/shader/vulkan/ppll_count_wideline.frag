#version 450
#extension GL_GOOGLE_include_directive : require

layout(early_fragment_tests) in;

#define ATLAS_CLIP_DISTANCE_EXTRA_USE_DISCARD 0
#define ATLAS_PPLL 1
#include "include/wideline_func1.glslinc"
#include "include/ppll_common.glslinc"

void main()
{
  if (atlas_should_reject_extra_clip_planes()) {
    return;
  }
  if (!fragment_discard_only()) {
    return;
  }
  ppllCountFragment();
}
