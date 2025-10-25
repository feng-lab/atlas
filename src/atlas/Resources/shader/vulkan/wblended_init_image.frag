#version 450
#extension GL_GOOGLE_include_directive : require

// Weighted Blended OIT init for image layers
// Outputs:
//  - FragAccum (location=0): weighted premultiplied color
//  - FragTrans (location=1): alpha in .x for transmittance accumulation

layout(location = 0) out vec4 FragAccum;
layout(location = 1) out vec4 FragTrans;

#include "include/lighting.glslinc"
#include "include/copyimage_func.glslinc"

// Use lighting UBO for depth transform constants

void main()
{
  vec4 c; float d;
  fragment_func(c, d);

  // Compute view-space depth and weight as in geometry WB init shaders
  float viewDepth = uLighting.ze_to_zw_a / (d - uLighting.ze_to_zw_b);
  float weight = clamp(0.03 / (1e-5 + pow(viewDepth * 0.005 * uLighting.weighted_blended_depth_scale, 4.0)), 1e-2, 3e3);

  FragAccum = c * weight;
  FragTrans = vec4(c.a, 0.0, 0.0, 0.0);
}
