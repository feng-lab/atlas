#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex0;
layout(set = 0, binding = 1) uniform sampler2D ColorTex1;


layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 SumColor = texelFetch(ColorTex0, p, 0);
  vec2 nandd    = texelFetch(ColorTex1, p, 0).xy;

  float n = nandd.x;
  if (n == 0.0) discard;

  vec3 AvgColor = SumColor.rgb / max(SumColor.a, 1e-6);
  float AvgAlpha = SumColor.a / n;
  float T = pow(1.0 - AvgAlpha, n);
  FragData0.a = 1.0 - T;
  FragData0.rgb = AvgColor * FragData0.a;
  gl_FragDepth = nandd.y / n;
}
