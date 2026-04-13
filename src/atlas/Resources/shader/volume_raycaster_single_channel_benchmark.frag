#if GLSL_VERSION < 130
uniform vec2 screen_dim_RCP;
#endif
uniform float sampling_rate;
#ifdef ISO
uniform float iso_value;
#endif
#ifdef LOCAL_MIP
uniform float local_MIP_threshold;
#endif
uniform float ze_to_zw_a;
uniform float ze_to_zw_b;
#ifdef SCREEN_SPACE_AUDIT_OUTPUT
uniform float ze_to_screen_pixel_voxel_size;
uniform float selected_voxel_world_size;
#endif

#ifndef ATLAS_ANALYTIC_RAY_SETUP
uniform sampler2DArray ray_entry_exit_tex_coord;
#endif

uniform sampler3D volume_1;
uniform vec3 volume_dimensions_1;
uniform sampler1D transfer_function_1;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;
#else
#define FragData0 gl_FragData[0]
#endif

vec4 compositeDVR(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0)
    rayDepth = currentRayLength;

  vec4 result = vec4(0.0);

  result.a = curResult.a + (1.0 -curResult.a) * color.a;
  result.rgb = (curResult.rgb * curResult.a + (1.0 - curResult.a) * color.a * color.rgb) / max(result.a, 1e-6);

  return result;
}

vec4 compositeISO(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth, in float isoValue)
{
  vec4 result = curResult;
  float epsilon = 0.02;
  if (color.a >= isoValue-epsilon && color.a <= isoValue+epsilon) {
    result = color;
    result.a = 1.0;
    rayDepth = currentRayLength;
  }
  return result;
}

vec4 compositeXRay(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0)
    rayDepth = currentRayLength;
  return curResult + color;
}

#ifdef SCREEN_SPACE_AUDIT_OUTPUT
float desiredVoxelSizeAtCurrentRayLength(in float zeFront, in float zeBack, in float currentRayLength)
{
  return mix(zeFront, zeBack, currentRayLength) * ze_to_screen_pixel_voxel_size;
}

void markAuditContribution(in float selectedVoxelSize,
                           in float desiredVoxelSize,
                           inout float contributingSampleCount,
                           inout float insufficientSampleCount,
                           inout float level0SampleCount,
                           inout float level0LimitedSampleCount)
{
  contributingSampleCount += 1.0;
  level0SampleCount += 1.0;
  if (selectedVoxelSize > desiredVoxelSize) {
    insufficientSampleCount += 1.0;
    level0LimitedSampleCount += 1.0;
  }
}

bool auditColorContributes(in vec4 color)
{
#ifdef ISO
  float epsilon = 0.02;
  return color.a >= iso_value - epsilon && color.a <= iso_value + epsilon;
#else
  return color.a > 0.0;
#endif
}
#endif

void main()
{
  vec4 entryTexCoordAndZ;
  vec4 exitTexCoordAndZ;
#ifdef ATLAS_ANALYTIC_RAY_SETUP
  bool hasRaySegment = atlasComputeAnalyticRaySegment(gl_FragCoord.xy, entryTexCoordAndZ, exitTexCoordAndZ);
#else
  entryTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 0), 0);
  exitTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 1), 0);
  bool hasRaySegment = !all(equal(entryTexCoordAndZ.xyz, exitTexCoordAndZ.xyz));
#endif
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (!hasRaySegment) {
    discard;
  } else {
    vec4 result = vec4(0.0);

#ifdef MIP
    float ch1V = 0.0;
#endif
#ifdef SCREEN_SPACE_AUDIT_OUTPUT
    float contributingSampleCount = 0.0;
    float insufficientSampleCount = 0.0;
    float level0SampleCount = 0.0;
    float level0LimitedSampleCount = 0.0;
#endif

    vec3 numVoxels = abs((exitRayPosition - startRayPosition) * volume_dimensions_1);
    float numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
    float stepSize = 1.0 / (sampling_rate * numVoxel);

    float currentRayLength = 0.0;
    float rayDepth = -1.0;
    float zeFront = entryTexCoordAndZ.w;
    float zeBack = exitTexCoordAndZ.w;
    bool finished = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float voxel;
        vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);

#if GLSL_VERSION >= 130
        voxel = texture(volume_1, samplePos).r;
#else
        voxel = texture3D(volume_1, samplePos).r;
#endif

#ifdef MIP
#ifdef LOCAL_MIP
        if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
          finished = true;
        } else if (voxel > ch1V) {
#ifdef SCREEN_SPACE_AUDIT_OUTPUT
          markAuditContribution(selected_voxel_world_size,
                                desiredVoxelSizeAtCurrentRayLength(zeFront, zeBack, currentRayLength),
                                contributingSampleCount,
                                insufficientSampleCount,
                                level0SampleCount,
                                level0LimitedSampleCount);
#endif
          ch1V = voxel;
          rayDepth = currentRayLength;
        }
#else
        if (voxel > ch1V) {
#ifdef SCREEN_SPACE_AUDIT_OUTPUT
          markAuditContribution(selected_voxel_world_size,
                                desiredVoxelSizeAtCurrentRayLength(zeFront, zeBack, currentRayLength),
                                contributingSampleCount,
                                insufficientSampleCount,
                                level0SampleCount,
                                level0LimitedSampleCount);
#endif
          ch1V = voxel;
          rayDepth = currentRayLength;
        }
        finished = ch1V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        vec4 color = texture(transfer_function_1, voxel);
#else
        vec4 color = texture1D(transfer_function_1, voxel);
#endif

#ifdef SCREEN_SPACE_AUDIT_OUTPUT
        if (auditColorContributes(color)) {
          markAuditContribution(selected_voxel_world_size,
                                desiredVoxelSizeAtCurrentRayLength(zeFront, zeBack, currentRayLength),
                                contributingSampleCount,
                                insufficientSampleCount,
                                level0SampleCount,
                                level0LimitedSampleCount);
        }
#endif
        if (color.a > 0.0) {
          color.a /= sampling_rate;
          result = COMPOSITING(result, color, currentRayLength, rayDepth);
          if (result.a >= 1.0) {
            result.a = 1.0;
            finished = true;
          }
        }
#endif

        currentRayLength += stepSize;
        finished = finished || (currentRayLength > 1.0);
      }
    }

#ifdef MIP
#ifdef SCREEN_SPACE_AUDIT_OUTPUT
    result = vec4(contributingSampleCount, insufficientSampleCount, level0SampleCount, level0LimitedSampleCount);
#else
#if GLSL_VERSION >= 130
#ifdef RAW_MIP_OUTPUT
    result = vec4(ch1V, ch1V, ch1V, 1.0);
#else
    result = texture(transfer_function_1, ch1V);
#endif
#else
#ifdef RAW_MIP_OUTPUT
    result = vec4(ch1V, ch1V, ch1V, 1.0);
#else
    result = texture1D(transfer_function_1, ch1V);
#endif
#endif
#endif
#elif defined(SCREEN_SPACE_AUDIT_OUTPUT)
    result = vec4(contributingSampleCount, insufficientSampleCount, level0SampleCount, level0LimitedSampleCount);
#endif

#ifndef SCREEN_SPACE_AUDIT_OUTPUT
#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif
#endif

    if (rayDepth >= 0.0) {
      gl_FragDepth = ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + ze_to_zw_b;
    } else {
#ifdef RESULT_OPAQUE
      gl_FragDepth = ze_to_zw_a / exitTexCoordAndZ.w + ze_to_zw_b;
#else
      gl_FragDepth = 1.0;
#endif
    }

#ifndef SCREEN_SPACE_AUDIT_OUTPUT
    result.rgb *= result.a;
#endif
    FragData0 = result;
  }
}
