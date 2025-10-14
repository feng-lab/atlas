#version 450

layout(set = 0, binding = 0) uniform sampler2D ColorTex0; // accumulated color * weight
layout(set = 0, binding = 1) uniform sampler2D ColorTex1; // transmittance

#include "include/oit_params.glslinc"

layout(location = 0) out vec4 FragData0;

const float kWeightNumerator = 0.03;
const float kWeightClampMin = 1e-2;
const float kWeightClampMax = 3e3;
const float kAlphaEpsilon = 1e-6;
const float kWeightEpsilon = 1e-5;

void main()
{
  vec2 tc = gl_FragCoord.xy * oit.screen_dim_RCP;
  vec4 sumColor = texture(ColorTex0, tc);
  float transmittance = texture(ColorTex1, tc).r;

  float resolvedAlpha = clamp(1.0 - transmittance, 0.0, 1.0);
  if (resolvedAlpha <= kAlphaEpsilon) {
    discard;
  }

  float accumWeightedAlpha = sumColor.a;
  float weight = accumWeightedAlpha / resolvedAlpha;
  weight = clamp(weight, kWeightClampMin, kWeightClampMax);

  float depthTerm = kWeightNumerator / weight - kWeightEpsilon;
  float fragDepth = 1.0;
  if (depthTerm > 0.0 && oit.weighted_blended_depth_scale > 0.0) {
    float viewDepth = pow(depthTerm, 0.25) / (0.005 * oit.weighted_blended_depth_scale);
    if (viewDepth > 1e-5) {
      fragDepth = clamp(oit.ze_to_zw_a / viewDepth + oit.ze_to_zw_b, 0.0, 1.0);
    }
  }

  float denom = clamp(sumColor.a, 1e-4, 5e4);
  vec3 resolvedColor = sumColor.rgb / denom * resolvedAlpha;
  FragData0 = vec4(resolvedColor, resolvedAlpha);
  gl_FragDepth = fragDepth;
}
