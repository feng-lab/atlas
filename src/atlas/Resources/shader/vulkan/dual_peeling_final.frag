#version 450

layout(set = 0, binding = 0) uniform sampler2D DepthTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderTex;
layout(set = 0, binding = 2) uniform sampler2D BackBlenderTex;

#include "include/oit_params.glslinc"

layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 tc = gl_FragCoord.xy * oit.screen_dim_RCP;
  vec4 frontColor = texture(FrontBlenderTex, tc);
  vec4 backColor  = texture(BackBlenderTex, tc);
  gl_FragDepth = -texture(DepthTex, tc).x;

  // premultiplied alpha combine: front + (1 - front.a) * back
  FragData0 = frontColor + (1.0 - frontColor.a) * backColor;
}
