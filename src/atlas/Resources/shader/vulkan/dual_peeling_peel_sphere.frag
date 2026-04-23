#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform DDPPeelPC {
  uint ddpDepthBlender;
  uint ddpFrontBlender;
} ddppc;


layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

// DDP indirect-count: match GL's occlusion query on the back-temp blend pass.
layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#define ATLAS_CLIP_DISTANCE_EXTRA_USE_DISCARD 0
#define ATLAS_PPLL 1
#include "include/sphere_func.glslinc"

void main()
{
  // Avoid discard in OIT shaders that use SSBO atomics. Emit no-op outputs for
  // miss fragments so MAX blending preserves existing values.
  if (atlas_should_reject_extra_clip_planes()) {
    FragData0.xy = vec2(-1.0);
    FragData1 = vec4(0.0);
    FragData2 = vec4(0.0);
    return;
  }
  vec4 color;
  float fragDepth;
  if (!fragment_func(color, fragDepth)) {
    FragData0.xy = vec2(-1.0);
    FragData1 = vec4(0.0);
    FragData2 = vec4(0.0);
    return;
  }
  gl_FragDepth = fragDepth;

  ivec2 p = ivec2(gl_FragCoord.xy);
  vec2 depthBlender = texelFetch(atlas_bindlessSampler2DNearest(ddppc.ddpDepthBlender), p, 0).xy;
  vec4 forwardTemp = texelFetch(atlas_bindlessSampler2DNearest(ddppc.ddpFrontBlender), p, 0);

  FragData1 = forwardTemp;
  FragData2 = vec4(0.0);

  float nearestDepth = -depthBlender.x;
  float farthestDepth = depthBlender.y;

  if (fragDepth < nearestDepth || fragDepth > farthestDepth) {
    FragData0.xy = vec2(-1.0);
    return;
  }
  if (fragDepth > nearestDepth && fragDepth < farthestDepth) {
    FragData0.xy = vec2(-fragDepth, fragDepth);
    return;
  }

  FragData0.xy = vec2(-1.0);
  if (fragDepth == nearestDepth) {
    FragData1 = forwardTemp + (1.0 - forwardTemp.a) * color;
  } else {
    FragData2 += color;
    if (color.a != 0.0) {
      atomicOr(ddp_flag.changed, 1u);
    }
  }
}
