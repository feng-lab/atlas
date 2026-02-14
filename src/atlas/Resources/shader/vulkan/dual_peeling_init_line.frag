#version 450
#extension GL_GOOGLE_include_directive : require

// Thin-line DDP init.
// Outputs the initial min/max depth range into the depth blender and stores
// -minDepth into depthTex for the final composite pass.
//
// Uses device depth (gl_FragCoord.z) plus (optional) extra clip-plane distances
// when XYZ cuts exceed the fixed-function clip-distance budget.

layout(location = 0) out vec4 FragData0; // depth blender (RG32F in the DDP RT)
layout(location = 1) out vec4 FragData1; // depthTex (R32F in the DDP RT)

#include "include/clip_distance_extra_frag.glslinc"

void main()
{
  atlas_discard_extra_clip_planes();
  const float fragDepth = gl_FragCoord.z;
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}
