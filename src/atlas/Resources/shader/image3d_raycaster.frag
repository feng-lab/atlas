#if GLSL_VERSION < 130
#extension GL_EXT_gpu_shader4 : enable
#endif

uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform sampler3D image_cache;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform vec3 image_address_to_normalized_texture_coord;

uniform float sampling_rate;
#ifdef ISO
uniform float iso_value;
#endif
#ifdef LOCAL_MIP
uniform float local_MIP_threshold;
#endif
uniform float ze_to_zw_a;
uniform float ze_to_zw_b;
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_entry_eye_coord;
uniform sampler2D ray_exit_tex_coord;
uniform sampler2D ray_exit_eye_coord;

uniform sampler2D last_ray_depth;
uniform sampler2D last_color;

uniform sampler3D volume;
uniform sampler1D transfer_function;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
out vec4 FragData1;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#define FragData1 gl_FragData[1]
#endif

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

#ifdef MIP
void sampleVolume(in vec3 startRayPosition, in vec3 rayVector, in float stepSize,
  inout float currentRayLength, inout bool finished, inout float ch1V, inout float rayDepth)
#else
void sampleVolume(in vec3 startRayPosition, in vec3 rayVector, in float stepSize,
  inout float currentRayLength, inout bool finished, inout vec4 result, inout float rayDepth)
#endif
{
  for (int loop0=0; !finished && loop0<255; loop0++) {
    for (int loop1=0; !finished && loop1<255; loop1++) {
      vec3 samplePos = startRayPosition + currentRayLength * rayVector;

      #if GLSL_VERSION >= 130
      float voxel = texture(volume, samplePos).r;
      #else
      float voxel = texture3D(volume, samplePos).r;
      #endif


      #ifdef MIP
      #ifdef LOCAL_MIP
      if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
        finished = true;
      } else if (voxel > ch1V) {
        ch1V = voxel;
        rayDepth = currentRayLength;
      }
      #else
      if (voxel > ch1V) {
        ch1V = voxel;
        rayDepth = currentRayLength;
      }
      finished = ch1V >= 1.0;
      #endif
      #else
      #if GLSL_VERSION >= 130
      vec4 color = texture(transfer_function, voxel);
      #else
      vec4 color = texture1D(transfer_function, voxel);
      #endif

      if (color.a > 0.0) {
        color.a / sampling_rate;
        result = COMPOSITING(result, color, currentRayLength, rayDepth);
        if (result.a >= 1.0) {
          result.a = 1.0;
          finished = true;
        }
      }
      #endif // MIP

      currentRayLength += stepSize;
      finished = finished || (currentRayLength > 1.0);
    }
  }
}

#ifdef MIP
void sampleBlock(in uvec3 pageTableEntry, in int curLevel, in uvec3 pageTableCoord,
  in vec3 startRayPosition, in vec3 rayVector, in float stepSize,
  inout float currentRayLength, inout vec3 samplePos, inout bool finished, inout float ch1V, inout float rayDepth)
#else
void sampleBlock(in uvec3 pageTableEntry, in int curLevel, in uvec3 pageTableCoord,
  in vec3 startRayPosition, in vec3 rayVector, in float stepSize,
  inout float currentRayLength, inout vec3 samplePos, inout bool finished, inout vec4 result, inout float rayDepth)
#endif
{
  bool blockFinished = false;
  vec3 voxelAddress;
#if GLSL_VERSION >= 130
  vec3 fFracVoxelCoord = modf(samplePos * image_dimensions[curLevel], voxelAddress);
  uvec3 voxelCoord = uvec3(voxelAddress);
#else
  uvec3 voxelCoord = uvec3(samplePos * image_dimensions[curLevel]);
  vec3 fFracVoxelCoord = samplePos * image_dimensions[curLevel] - vec3(voxelCoord);
#endif
  for (int loop0=0; !blockFinished && loop0<255; loop0++) {
    //uvec3 voxelAddress = pageTableEntry.xyz + voxelCoord % image_block_size;
    //float voxel = texelFetch(image_cache, voxelAddress, 0).r;
    voxelAddress = pageTableEntry + voxelCoord % image_block_size + fFracVoxelCoord + 2.0;
#if GLSL_VERSION >= 130
    float voxel = texture(image_cache, (voxelAddress)*image_address_to_normalized_texture_coord).r;
#else
    float voxel = texture3D(image_cache, (voxelAddress)*image_address_to_normalized_texture_coord).r;
#endif

#ifdef MIP
#ifdef LOCAL_MIP
    if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
      finished = true;
    } else if (voxel > ch1V) {
      ch1V = voxel;
      rayDepth = currentRayLength;
    }
#else
    if (voxel > ch1V) {
      ch1V = voxel;
      rayDepth = currentRayLength;
    }
    finished = ch1V >= 1.;
#endif
#else
#if GLSL_VERSION >= 130
    vec4 color = texture(transfer_function, voxel);
#else
    vec4 color = texture1D(transfer_function, voxel);
#endif

    if (color.a > 0.0) {
      color.a /= sampling_rate;
      result = COMPOSITING(result, color, currentRayLength, rayDepth);
      if (result.a >= 1.0) {
        result.a = 1.0;
        finished = true;
      }
    }
#endif // MIP
    currentRayLength += stepSize;

    samplePos = startRayPosition + currentRayLength * rayVector;
#if GLSL_VERSION >= 130
    fFracVoxelCoord = modf(samplePos * image_dimensions[curLevel], voxelAddress);
    voxelCoord = uvec3(voxelAddress);
#else
    voxelCoord = uvec3(samplePos * image_dimensions[curLevel]);
    fFracVoxelCoord = samplePos * image_dimensions[curLevel] - vec3(voxelCoord);
#endif
    blockFinished = finished || voxelCoord / image_block_size != pageTableCoord || currentRayLength > 1.0;
  }
}

#define UNMAPPED 0
#define EMPTY 40000
#define UINTMAX 4294967295U

void main()
{
#if GLSL_VERSION >= 130
  vec2 lastRayDepth = texelFetch(last_ray_depth, ivec2(gl_FragCoord.xy), 0).xy;
  vec4 result = texelFetch(last_color, ivec2(gl_FragCoord.xy), 0);
#else
  vec2 lastRayDepth = texelFetch2D(last_ray_depth, ivec2(gl_FragCoord.xy), 0).xy;
  vec4 result = texelFetch2D(last_color, ivec2(gl_FragCoord.xy), 0);
#endif
  float currentRayLength = lastRayDepth.x;
  float rayDepth = lastRayDepth.y;
  if (currentRayLength >= 1.0) {
    FragData0 = result;
    FragData1.xy = lastRayDepth;
    return;
  }
  if (currentRayLength == 0.0) {
    rayDepth = -1.0;
  }

#if GLSL_VERSION >= 130
  vec4 entryTexCoordAndZ = texelFetch(ray_entry_tex_coord, ivec2(gl_FragCoord.xy), 0);
  vec4 exitTexCoordAndZ = texelFetch(ray_exit_tex_coord, ivec2(gl_FragCoord.xy), 0);
#else
  vec4 entryTexCoordAndZ = texelFetch2D(ray_entry_tex_coord, ivec2(gl_FragCoord.xy), 0);
  vec4 exitTexCoordAndZ = texelFetch2D(ray_exit_tex_coord, ivec2(gl_FragCoord.xy), 0);
#endif
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    // vec4 result = vec4(0.0);
#ifdef MIP
    float ch1V = result.r;
#endif

    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
    float zeFront = texelFetch(ray_entry_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
    float zeBack = texelFetch(ray_exit_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
#else
    float zeFront = texelFetch2D(ray_entry_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
    float zeBack = texelFetch2D(ray_exit_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
#endif
    int curLevel = 0;

    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * image_dimensions[curLevel]);
    float stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

    // float currentRayLength = 0.0;
    // float rayDepth = -1.0;

    uvec3 pageDirAddress = uvec3(UINTMAX,UINTMAX,UINTMAX);
    uvec4 pageDirEntry = uvec4(UINTMAX,UINTMAX,UINTMAX,UINTMAX);

    bool finished = false;
    bool hitMissedBlock = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * ze_to_screen_pixel_voxel_size;
        while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
          ++curLevel;
          numVoxels = abs(rayVector * image_dimensions[curLevel]);
          stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
        }

        if (curLevel + 1 == LEVEL_COUNT) {
#ifdef MIP
          sampleVolume(startRayPosition, rayVector, stepSize,
                       currentRayLength, finished, ch1V, rayDepth);
#else
          sampleVolume(startRayPosition, rayVector, stepSize,
                       currentRayLength, finished, result, rayDepth);
#endif
          // finished = true;  // should be true
          break;
        }

        vec3 samplePos = startRayPosition + currentRayLength * rayVector;

        uvec3 pageTableCoord = uvec3(samplePos * image_dimensions[curLevel]) / image_block_size;
        uvec3 curPageDirAddress = page_directory_bases[curLevel] + pageTableCoord / page_table_block_size;
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
#if GLSL_VERSION >= 130
          pageDirEntry = texelFetch(page_directory, ivec3(pageDirAddress), 0);
#else
          pageDirEntry = texelFetch3D(page_directory, ivec3(pageDirAddress), 0);
#endif
        }
        uint pagingFlag = pageDirEntry.w;
        if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
#if GLSL_VERSION >= 130
          uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
#else
          uvec4 pageTableEntry = texelFetch3D(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
#endif
          pagingFlag = pageTableEntry.w;
          if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
#ifdef MIP
            sampleBlock(pageTableEntry.xyz, curLevel, pageTableCoord,
              startRayPosition, rayVector, stepSize,
              currentRayLength, samplePos, finished, ch1V, rayDepth);
#else
            sampleBlock(pageTableEntry.xyz, curLevel, pageTableCoord,
              startRayPosition, rayVector, stepSize,
              currentRayLength, samplePos, finished, result, rayDepth);
#endif
          } else {
            // skip empty space page table entry recursive
            if (pagingFlag == UNMAPPED) {
              hitMissedBlock = true;
            } else { // empty block
              int nextNonEmptyLevel = curLevel + 1;
              uint testPagingFlag = EMPTY;
              while (testPagingFlag == EMPTY && nextNonEmptyLevel + 1 < LEVEL_COUNT) {
                uvec3 testVoxelCoord = uvec3(samplePos * image_dimensions[nextNonEmptyLevel]);
                uvec3 testPageTableCoord = testVoxelCoord / image_block_size;

#if GLSL_VERSION >= 130
                uvec4 testPageDirEntry = texelFetch(page_directory, ivec3(page_directory_bases[nextNonEmptyLevel] + testPageTableCoord / page_table_block_size), 0);
#else
                uvec4 testPageDirEntry = texelFetch3D(page_directory, ivec3(page_directory_bases[nextNonEmptyLevel] + testPageTableCoord / page_table_block_size), 0);
#endif
                testPagingFlag = testPageDirEntry.w;
                if (testPagingFlag != UNMAPPED && testPagingFlag != EMPTY) {
#if GLSL_VERSION >= 130
                  testPagingFlag = texelFetch(page_table_cache, ivec3(testPageDirEntry.xyz + testPageTableCoord % page_table_block_size), 0).w;
#else
                  testPagingFlag = texelFetch3D(page_table_cache, ivec3(testPageDirEntry.xyz + testPageTableCoord % page_table_block_size), 0).w;
#endif
                }
                ++nextNonEmptyLevel;
              }

              uvec3 prevBlock = uvec3(samplePos * image_dimensions[nextNonEmptyLevel-1]) / image_block_size;
              numVoxels = abs(rayVector * image_dimensions[nextNonEmptyLevel-1]);
              float testStepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
              do {
                currentRayLength += testStepSize;
              } while (uvec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[nextNonEmptyLevel-1]) / image_block_size == prevBlock && currentRayLength < 1.0);
            }
          }
        } else {
          if (pagingFlag == EMPTY) {
            do { // skip empty space page directory entry
              currentRayLength += stepSize;
            } while (page_directory_bases[curLevel] + uvec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size / page_table_block_size == pageDirAddress && currentRayLength < 1.0);
          } else { // pagingFlag == UNMAPPED
            hitMissedBlock = true;
          }
        }

        finished = finished || hitMissedBlock || (currentRayLength > 1.0);
      } // for
    }

    if (hitMissedBlock && currentRayLength < 1.0) {
#ifdef MIP
      FragData0.x = ch1V;
#else
      FragData0 = result;
#endif
      FragData1.xy = vec2(currentRayLength, rayDepth);
      return;
    }

#ifdef MIP
#if GLSL_VERSION >= 130
    result = texture(transfer_function, ch1V);
#else
    result = texture1D(transfer_function, ch1V);
#endif
#endif // MIP

#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif

    float fragDepth;
    if (rayDepth >= 0.0) {
      fragDepth = ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + ze_to_zw_b;
    } else {
#ifdef RESULT_OPAQUE
      fragDepth = entryTexCoordAndZ.w;
#else
      fragDepth = 1.0;
#endif
    }

    result.rgb *= result.a;
    FragData0 = result;
    FragData1.xy = vec2(1.0, fragDepth);
  }
}

