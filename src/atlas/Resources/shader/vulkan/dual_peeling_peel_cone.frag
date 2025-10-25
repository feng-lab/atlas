#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D DepthBlenderTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderTex;


layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

layout(set = 3, binding = 1) buffer DDPFlag { uint changed; } ddp_flag;

#include "include/matrices_material.glslinc"
#include "include/cone_func.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;

  ivec2 p = ivec2(gl_FragCoord.xy);
  vec2 depthBlender = texelFetch(DepthBlenderTex, p, 0).xy;
  vec4 forwardTemp = texelFetch(FrontBlenderTex, p, 0);

  FragData1 = forwardTemp; // pass-through by default with MAX blending
  FragData2 = vec4(0.0);

  float nearestDepth = -depthBlender.x;
  float farthestDepth = depthBlender.y;

  if (fragDepth < nearestDepth || fragDepth > farthestDepth) {
    FragData0.xy = vec2(-1.0); // -MAX_DEPTH sentinel
    return;
  }

  if (fragDepth > nearestDepth && fragDepth < farthestDepth) {
    FragData0.xy = vec2(-fragDepth, fragDepth);
    atomicOr(ddp_flag.changed, 1u);
    return;
  }

  FragData0.xy = vec2(-1.0); // mark as peeled
  if (fragDepth == nearestDepth) {
    FragData1 = forwardTemp + (1.0 - forwardTemp.a) * color; // premultiplied alpha
  } else {
    FragData2 += color;
  }
  atomicOr(ddp_flag.changed, 1u);
}
