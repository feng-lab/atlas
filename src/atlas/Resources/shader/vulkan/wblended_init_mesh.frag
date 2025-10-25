#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

// Push constants no longer required; use lighting UBO

#include "include/lighting.glslinc"
#include "include/mesh_func.glslinc"

void main()
{
  vec4 color; float fragDepth;
  fragment_func(color, fragDepth);
  gl_FragDepth = fragDepth;

  float viewDepth = uLighting.ze_to_zw_a / (fragDepth - uLighting.ze_to_zw_b);
  float weight = clamp(0.03 / (1e-5 + pow(viewDepth * 0.005 * uLighting.weighted_blended_depth_scale, 4.0)), 1e-2, 3e3);
  FragData0 = color * weight;
  FragData1.x = color.a;
}
