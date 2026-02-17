#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform WAvgFinalPC {
  uint accum_tex;
  uint moments_tex;
} wpc;

layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 SumColor = texelFetch(atlas_bindlessSampler2DNearest(wpc.accum_tex), p, 0);
  vec2 nandd    = texelFetch(atlas_bindlessSampler2DNearest(wpc.moments_tex), p, 0).xy;

  float n = nandd.x;
  if (n == 0.0) discard;

  vec3 AvgColor = SumColor.rgb / max(SumColor.a, 1e-6);
  float AvgAlpha = SumColor.a / n;
  float T = pow(1.0 - AvgAlpha, n);
  FragData0.a = 1.0 - T;
  FragData0.rgb = AvgColor * FragData0.a;
  gl_FragDepth = nandd.y / n;
}
