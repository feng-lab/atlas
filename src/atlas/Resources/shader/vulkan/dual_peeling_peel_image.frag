#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0; // depth blender
layout(location = 1) out vec4 FragData1; // front blender
layout(location = 2) out vec4 FragData2; // back temp

// DDP indirect-count: mark when this pass updates any pixel
layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#define ATLAS_PPLL 1
#include "include/copyimage_func.glslinc"

void main()
{
  // Avoid discard in OIT shaders that use SSBO atomics. Emit no-op outputs for
  // transparent/empty fragments so MAX blending preserves existing values.
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
  vec2 depthBlender = texelFetch(atlas_bindlessSampler2DNearest(copy_pc.ddpDepthBlender), p, 0).xy;
  vec4 forwardTemp = texelFetch(atlas_bindlessSampler2DNearest(copy_pc.ddpFrontBlender), p, 0);

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
