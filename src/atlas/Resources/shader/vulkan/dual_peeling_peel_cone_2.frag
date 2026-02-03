#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D DepthBlenderTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderTex;

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

// DDP indirect-count: mark when this pass updates any pixel
layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#include "include/matrices_material.glslinc"
#define ATLAS_PPLL 1
#include "include/cone_func_2.glslinc"

void main()
{
  vec4 color;
  float depth;
  if (!fragment_func(color, depth)) {
    FragData0.xy = vec2(-1.0);
    FragData1 = vec4(0.0);
    FragData2 = vec4(0.0);
    return;
  }
  gl_FragDepth = depth;

  ivec2 p = ivec2(gl_FragCoord.xy);
  vec2 depthBlender = texelFetch(DepthBlenderTex, p, 0).xy;
  vec4 forwardTemp = texelFetch(FrontBlenderTex, p, 0);

  FragData1 = forwardTemp;
  FragData2 = vec4(0.0);

  float nearestDepth = -depthBlender.x;
  float farthestDepth = depthBlender.y;

  if (depth < nearestDepth || depth > farthestDepth) {
    FragData0.xy = vec2(-1.0);
    return;
  }

  if (depth > nearestDepth && depth < farthestDepth) {
    FragData0.xy = vec2(-depth, depth);
    atomicOr(ddp_flag.changed, 1u);
    return;
  }

  FragData0.xy = vec2(-1.0);
  if (depth == nearestDepth) {
    FragData1 = forwardTemp + (1.0 - forwardTemp.a) * color;
  } else {
    FragData2 += color;
  }
  atomicOr(ddp_flag.changed, 1u);
}
