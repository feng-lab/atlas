#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex0; // accumulated color * weight
layout(set = 0, binding = 1) uniform sampler2D ColorTex1; // transmittance

#include "include/oit_params.glslinc"

layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 tc = gl_FragCoord.xy * oit.screen_dim_RCP;
  vec4 sumColor = texture(ColorTex0, tc);
  float transmittance = texture(ColorTex1, tc).r;

  float a = 1.0 - transmittance;
  FragData0 = vec4(sumColor.rgb / clamp(sumColor.a, 1e-4, 5e4) * a, a);
}
