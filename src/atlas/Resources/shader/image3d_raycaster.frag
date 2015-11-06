uniform sampler3D page_directory;
uniform ivec3 page_directory_dimensions[LEVEL_COUNT];
uniform sampler3D page_table_cache;
uniform ivec3 page_table_dimensions[LEVEL_COUNT];
uniform ivec3 page_table_block_size = ivec3(32, 32, 32);
uniform sampler3D voxel_cache;
uniform ivec3 voxel_dimensions[LEVEL_COUNT];
uniform ivec3 voxel_block_size = ivec3(32, 32, 32);

uniform vec2 screen_dim_RCP;
uniform float sampling_rate;
#ifdef ISO
uniform float iso_value;
#endif
#ifdef LOCAL_MIP
uniform float local_MIP_threshold;
#endif
uniform float ze_to_zw_a;
uniform float ze_to_zw_b;

uniform sampler2D ray_entry_points;
uniform sampler2D ray_entry_points_depth;
uniform sampler2D ray_exit_points;
uniform sampler2D ray_exit_points_depth;

uniform VolumeStruct volume_struct_1;
uniform TF_SAMPLER_TYPE_1 transfer_function_1;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
layout(location = 1) out uvec4 FragData1;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
out uvec4 FragData1;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#define FragData1 gl_FragData[1]
#endif

vec4 applyTF(in sampler1D tex, in float intensity)
{
#if GLSL_VERSION >= 130
#if defined(MIP)
  return texture(tex, intensity);
#else
  vec4 res = texture(tex, intensity);
  res.a = res.a / sampling_rate;
  return res;
#endif
#else
#if defined(MIP)
  return texture1D(tex, intensity);
#else
  vec4 res = texture1D(tex, intensity);
  res.a = res.a / sampling_rate;
  return res;
#endif
#endif
}

vec4 compositeDVR(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0)
    rayDepth = currentRayLength;

  vec4 result = vec4(0.0);

  result.a = curResult.a + (1.0 -curResult.a) * color.a;
  result.rgb = (curResult.rgb * curResult.a + (1.0 - curResult.a) * color.a * color.rgb) / result.a;

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

int resolutionLevelOfCurrentSample(vec3 samplePos, int lastLevel)
{
  int res = lastLevel;

}

void main()
{
  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;
#if GLSL_VERSION >= 130
  vec3 startRayPosition = texture(ray_entry_points, texCoords).xyz;
  vec3 exitRayPosition = texture(ray_exit_points, texCoords).xyz;
#else
  vec3 startRayPosition = texture2D(ray_entry_points, texCoords).xyz;
  vec3 exitRayPosition = texture2D(ray_exit_points, texCoords).xyz;
#endif

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    vec4 result = vec4(0.0);

#ifdef MIP
    float ch1V = 0.0;
#endif

#ifdef LOCAL_MIP
    bool ch1Done = false;
#endif

    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
    float zwFront = texture(ray_entry_points_depth, texCoords).r;
    float zwBack = texture(ray_exit_points_depth, texCoords).r;
#else
    float zwFront = texture2D(ray_entry_points_depth, texCoords).r;
    float zwBack = texture2D(ray_exit_points_depth, texCoords).r;
#endif
    float zeFront = ze_to_zw_a / (zwFront - ze_to_zw_b);
    float zeBack = ze_to_zw_a / (zwBack - ze_to_zw_b);

    vec3 dimension = volume_struct_1.dimensions;
    vec3 rayVector = exitRayPosition - startRayPosition;
    float maxRayLength = length(rayVector);
    float stepSize = maxRayLength / (sampling_rate * length(normalize(rayVector) * dimension));

    float currentRayLength = 0.0;
    float rayDepth = -1.0;
    bool finished = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float voxel;
        vec4 color = vec4(0.0);
        vec4 chColor;
        vec3 samplePos = startRayPosition + currentRayLength / maxRayLength * rayVector;
        bool saturated = true;

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_1.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_1.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch1Done) {
          if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
            ch1Done = true;
          } else if (voxel > ch1V) {
            ch1V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch1Done;
#else
        if (voxel > ch1V) {
          rayDepth = currentRayLength;
          ch1V = voxel;
        }
        saturated = saturated && ch1V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_1, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP


#ifdef MIP
        finished = saturated;
#else
        if (color.a > 0.0) {
          result = COMPOSITING(result, color, currentRayLength, rayDepth);
        }

        if (result.a >= 1.0) {
          result.a = 1.0;
          finished = true;
        }
#endif // MIP
        currentRayLength += stepSize;
        finished = finished || (currentRayLength > maxRayLength);
      }
    }

#ifdef MIP
  result = max(result, applyTF(transfer_function_1, ch1V));
#endif // MIP

#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif


    if (rayDepth >= 0.0) {
      float ze = zeFront + rayDepth / maxRayLength * (zeBack-zeFront);
      gl_FragDepth = ze_to_zw_a / ze + ze_to_zw_b;
    } else {
#ifdef RESULT_OPAQUE
#if GLSL_VERSION >= 130
      gl_FragDepth = texture(ray_entry_points_depth, texCoords).r;
#else
      gl_FragDepth = texture2D(ray_entry_points_depth, texCoords).r;
#endif
#else
      gl_FragDepth = 1.0;
#endif
    }

    result.rgb *= result.a;
    FragData0 = result;
  }
}

