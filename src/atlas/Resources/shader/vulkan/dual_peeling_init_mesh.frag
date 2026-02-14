#version 450
#extension GL_GOOGLE_include_directive : require

// Mesh DDP init only needs device depth (gl_FragCoord.z) plus (optional) extra
// clip-plane distances when XYZ cuts exceed the fixed-function clip-distance
// budget. Avoid pulling in the full mesh shading path.

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/clip_distance_extra_frag.glslinc"

void main()
{
  atlas_discard_extra_clip_planes();
  const float fragDepth = gl_FragCoord.z;
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}
