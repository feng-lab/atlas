#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 3) uniform sampler2D DepthBlenderTex;
layout(set = 0, binding = 4) uniform sampler2D FrontBlenderTex;

layout(location = 0) out vec4 FragData0; // depth blender
layout(location = 1) out vec4 FragData1; // front blender
layout(location = 2) out vec4 FragData2; // back temp

#include "include/oit_params.glslinc"
#include "include/copyimage_func.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;

  vec2 depthBlender = texture(DepthBlenderTex, gl_FragCoord.xy * oit.screen_dim_RCP).xy;
  vec4 forwardTemp = texture(FrontBlenderTex, gl_FragCoord.xy * oit.screen_dim_RCP);

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
  }
}

