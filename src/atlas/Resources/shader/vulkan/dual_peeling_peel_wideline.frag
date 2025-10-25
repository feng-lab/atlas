#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 1) uniform sampler2D DepthBlenderTex;
layout(set = 0, binding = 2) uniform sampler2D FrontBlenderTex;


layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#include "include/wideline_func1.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;

  ivec2 p = ivec2(gl_FragCoord.xy);
  vec2 depthBlender = texelFetch(DepthBlenderTex, p, 0).xy;
  vec4 forwardTemp = texelFetch(FrontBlenderTex, p, 0);

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
