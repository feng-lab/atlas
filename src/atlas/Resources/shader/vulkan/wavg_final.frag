#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex0;
layout(set = 0, binding = 1) uniform sampler2D ColorTex1;

#include "include/oit_params.glslinc"

layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 tc = gl_FragCoord.xy * oit.screen_dim_RCP;
  vec4 SumColor = texture(ColorTex0, tc);
  vec2 nandd    = texture(ColorTex1, tc).xy;

  float n = nandd.x;
  if (n == 0.0) discard;

  vec3 AvgColor = SumColor.rgb / max(SumColor.a, 1e-6);
  float AvgAlpha = SumColor.a / n;
  float T = pow(1.0 - AvgAlpha, n);
  FragData0.a = 1.0 - T;
  FragData0.rgb = AvgColor * FragData0.a;
  gl_FragDepth = nandd.y / n;
}
