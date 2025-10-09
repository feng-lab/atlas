#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D DepthBlenderTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderTex;

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
layout(location = 2) out vec4 FragData2;

#include "include/matrices_material.glslinc"
#include "include/oit_params.glslinc"
#include "include/cone_func_2.glslinc"

void main()
{
  vec4 color;
  float depth;
  fragment_func(color, depth);
  gl_FragDepth = depth;

  vec2 depthBlender = texture(DepthBlenderTex, gl_FragCoord.xy * oit.screen_dim_RCP).xy;
  vec4 forwardTemp = texture(FrontBlenderTex, gl_FragCoord.xy * oit.screen_dim_RCP);

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
    return;
  }

  FragData0.xy = vec2(-1.0);
  if (depth == nearestDepth) {
    FragData1 = forwardTemp + (1.0 - forwardTemp.a) * color;
  } else {
    FragData2 += color;
  }
}
