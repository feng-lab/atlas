#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/matrices_material.glslinc"
#include "include/lighting.glslinc"
#include "include/cone_func_2.glslinc"

// Use lighting UBO for depth transform constants

void main()
{
  vec4 color;
  float depth;
  fragment_func(color, depth);
  gl_FragDepth = depth;

  float viewDepth = uLighting.ze_to_zw_a / (depth - uLighting.ze_to_zw_b);
  float weight = clamp(0.03 / (1e-5 + pow(viewDepth * 0.005 * uLighting.weighted_blended_depth_scale, 4.0)), 1e-2, 3e3);
  FragData0 = color * weight;
  FragData1.x = color.a;
}
