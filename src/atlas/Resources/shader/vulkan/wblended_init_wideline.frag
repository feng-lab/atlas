#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;


#include "include/oit_params.glslinc"
#include "include/wideline_func1.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;

  float viewDepth = oit.ze_to_zw_a / (fragDepth - oit.ze_to_zw_b);
  float weight = clamp(0.03 / (1e-5 + pow(viewDepth * 0.005 * oit.weighted_blended_depth_scale, 4.0)), 1e-2, 3e3);
  FragData0 = color * weight;
  FragData1.x = color.a;
}
