#version 450

// Ray modes: 0=DVR, 1=MIP, 2=ISO, 3=XRAY
layout(constant_id = 80) const int RAY_MODE = 0;
layout(constant_id = 81) const bool LOCAL_MIP = false;
layout(constant_id = 51) const bool RESULT_OPAQUE = false;

layout(set = 0, binding = 0) uniform sampler2DArray ray_entry_exit_tex_coord;
layout(set = 0, binding = 1) uniform sampler3D      volume_1;
layout(set = 0, binding = 2) uniform sampler2D      transfer_function_1;

// Push constants for ray params
layout(push_constant) uniform RayParams {
  float sampling_rate;
  float iso_value;
  float local_MIP_threshold;
  float ze_to_zw_a;
  float ze_to_zw_b;
} rp;

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
  vec4 entryTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 0), 0);
  vec4 exitTexCoordAndZ  = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 1), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition  = exitTexCoordAndZ.xyz;
  if (all(equal(startRayPosition, exitRayPosition))) { discard; }

  vec4 result = vec4(0.0);
  float ch1V = 0.0;

  vec3 numVoxels = abs((exitRayPosition - startRayPosition) * vec3(textureSize(volume_1, 0)));
  float numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
  float stepSize = 1.0 / (rp.sampling_rate * numVoxel);

  float currentRayLength = 0.0;
  float rayDepth = -1.0;
  bool finished = false;
  for (int loop0=0; !finished && loop0<255; ++loop0) {
    for (int loop1=0; !finished && loop1<255; ++loop1) {
      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
      float voxel = texture(volume_1, samplePos).r;

      if (RAY_MODE == 1) { // MIP
        if (LOCAL_MIP) {
          if (voxel <= ch1V && ch1V >= rp.local_MIP_threshold) { finished = true; }
          else if (voxel > ch1V) { ch1V = voxel; rayDepth = currentRayLength; }
        } else {
          if (voxel > ch1V) { ch1V = voxel; rayDepth = currentRayLength; }
          finished = ch1V >= 1.0;
        }
      } else {
        vec4 color = texture(transfer_function_1, vec2(voxel, 0.5));
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

  if (RAY_MODE == 1) result = texture(transfer_function_1, vec2(ch1V, 0.5));
  if (RESULT_OPAQUE) result.a = 1.0;

  // Depth
  if (rayDepth >= 0.0) {
    float zeFront = entryTexCoordAndZ.w;
    float zeBack  = exitTexCoordAndZ.w;
    gl_FragDepth = rp.ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + rp.ze_to_zw_b;
  } else {
    gl_FragDepth = RESULT_OPAQUE ? (rp.ze_to_zw_a / entryTexCoordAndZ.w + rp.ze_to_zw_b) : 1.0;
  }

  result.rgb *= result.a;
  FragData0 = result;
}
