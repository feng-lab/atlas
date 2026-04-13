#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

#define ATLAS_ANALYTIC_MAX_CLIP_PLANES 6

// Ray modes: 0=DVR, 1=MIP, 2=ISO, 3=XRAY
layout(constant_id = 80) const int RAY_MODE = 0;
layout(constant_id = 81) const bool LOCAL_MIP = false;
layout(constant_id = 51) const bool RESULT_OPAQUE = false;

// Push constants for ray params
layout(push_constant) uniform RayParams {
  float sampling_rate;
  float iso_value;
  float local_MIP_threshold;
  float ze_to_zw_a;
  float ze_to_zw_b;

  uint ray_entry_exit_tex_coord;
  uint volume_1;
  uint transfer_function_1;
} rp;

#include "include/analytic_ray_setup.glslinc"

layout(location = 0) out vec4 FragData0;

vec4 compositeDVR(vec4 curResult, vec4 color, float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0) rayDepth = currentRayLength;
  vec4 result = vec4(0.0);
  result.a = curResult.a + (1.0 - curResult.a) * color.a;
  result.rgb = (curResult.rgb * curResult.a + (1.0 - curResult.a) * color.a * color.rgb) / max(result.a, 1e-6);
  return result;
}

vec4 compositeISO(vec4 curResult, vec4 color, float currentRayLength, inout float rayDepth)
{
  vec4 result = curResult;
  float epsilon = 0.02;
  if (color.a >= rp.iso_value - epsilon && color.a <= rp.iso_value + epsilon) {
    result = color;
    result.a = 1.0;
    rayDepth = currentRayLength;
  }
  return result;
}

vec4 compositeXRay(vec4 curResult, vec4 color, float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0) rayDepth = currentRayLength;
  return curResult + color;
}

void main()
{
  vec4 entryTexCoordAndZ;
  vec4 exitTexCoordAndZ;
  if (!atlasFetchRaySegment(entryTexCoordAndZ, exitTexCoordAndZ, rp.ray_entry_exit_tex_coord)) {
    discard;
  }
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition  = exitTexCoordAndZ.xyz;

  vec4 result = vec4(0.0);
  float ch1V = 0.0;

  vec3 numVoxels =
    abs((exitRayPosition - startRayPosition) *
        vec3(textureSize(atlas_bindlessSampler3DLinearBorderZero(rp.volume_1), 0)));
  float numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
  float stepSize = 1.0 / (rp.sampling_rate * numVoxel);

  float currentRayLength = 0.0;
  float rayDepth = -1.0;
  bool finished = false;
  for (int loop0=0; !finished && loop0<255; ++loop0) {
    for (int loop1=0; !finished && loop1<255; ++loop1) {
      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
      float voxel = texture(atlas_bindlessSampler3DLinearBorderZero(rp.volume_1), samplePos).r;

      if (RAY_MODE == 1) { // MIP
        if (LOCAL_MIP) {
          if (voxel <= ch1V && ch1V >= rp.local_MIP_threshold) { finished = true; }
          else if (voxel > ch1V) { ch1V = voxel; rayDepth = currentRayLength; }
        } else {
          if (voxel > ch1V) { ch1V = voxel; rayDepth = currentRayLength; }
          finished = ch1V >= 1.0;
        }
      } else {
        vec4 color = texture(atlas_bindlessSampler2DLinear(rp.transfer_function_1), vec2(voxel, 0.5));
        if (color.a > 0.0) {
          color.a /= rp.sampling_rate;
          if (RAY_MODE == 2) result = compositeISO(result, color, currentRayLength, rayDepth);
          else if (RAY_MODE == 3) result = compositeXRay(result, color, currentRayLength, rayDepth);
          else result = compositeDVR(result, color, currentRayLength, rayDepth);
          if (result.a >= 1.0) { result.a = 1.0; finished = true; }
        }
      }

      currentRayLength += stepSize;
      finished = finished || (currentRayLength > 1.0);
    }
  }

  if (RAY_MODE == 1) result = texture(atlas_bindlessSampler2DLinear(rp.transfer_function_1), vec2(ch1V, 0.5));
  if (RESULT_OPAQUE) result.a = 1.0;

  // Depth
  if (rayDepth >= 0.0) {
    float zeFront = entryTexCoordAndZ.w;
    float zeBack  = exitTexCoordAndZ.w;
    gl_FragDepth = rp.ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + rp.ze_to_zw_b;
  } else {
    // No-hit silhouette (RESULT_OPAQUE): use exit depth so it occludes objects behind the
    // volume but does not hide geometry inside the volume footprint.
    gl_FragDepth = RESULT_OPAQUE ? (rp.ze_to_zw_a / exitTexCoordAndZ.w + rp.ze_to_zw_b) : 1.0;
  }

  result.rgb *= result.a;
  FragData0 = result;
}
