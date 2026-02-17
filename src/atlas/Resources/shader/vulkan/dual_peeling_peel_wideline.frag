#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#define ATLAS_CLIP_DISTANCE_EXTRA_USE_DISCARD 0
#define ATLAS_PPLL 1
#include "include/wideline_func1.glslinc"

void main()
{
  if (atlas_should_reject_extra_clip_planes()) {
    FragData0.xy = vec2(-1.0);
    FragData1 = vec4(0.0);
    FragData2 = vec4(0.0);
    return;
  }
  // Avoid discard in OIT shaders that use SSBO atomics (MoltenVK/driver stability).
  // When a fragment is not covered by the wide-line expansion, emit no-op
  // outputs so MAX blending leaves existing values unchanged and do not touch
  // the DDP changed flag.
  vec4 color;
  float fragDepth;
  if (!fragment_func(color, fragDepth)) {
    FragData0.xy = vec2(-1.0);
    FragData1 = vec4(0.0);
    FragData2 = vec4(0.0);
    return;
  }

  const ivec2 p = ivec2(gl_FragCoord.xy);
  const vec2 depthBlender = texelFetch(atlas_bindlessSampler2DNearest(wpc.ddpDepthBlender), p, 0).xy;
  const vec4 forwardTemp = texelFetch(atlas_bindlessSampler2DNearest(wpc.ddpFrontBlender), p, 0);

  FragData1 = forwardTemp; // pass-through by default with MAX blending
  FragData2 = vec4(0.0);   // back temp

  float nearestDepth = -depthBlender.x;
  float farthestDepth = depthBlender.y;

  if (fragDepth < nearestDepth || fragDepth > farthestDepth) {
    FragData0.xy = vec2(-1.0);
    return;
  }
  if (fragDepth > nearestDepth && fragDepth < farthestDepth) {
    FragData0.xy = vec2(-fragDepth, fragDepth);
    atomicOr(ddp_flag.changed, 1u);
    return;
  }

  FragData0.xy = vec2(-1.0);
  if (fragDepth == nearestDepth) {
    FragData1 = forwardTemp + (1.0 - forwardTemp.a) * color;
  } else {
    FragData2 += color;
  }
  atomicOr(ddp_flag.changed, 1u);
}
