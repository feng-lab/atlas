uniform isampler3D page_directory;
uniform ivec3 page_directory_bases[LEVEL_COUNT];
uniform isampler3D page_table_cache;
uniform ivec3 page_table_block_size = ivec3(32, 32, 32);
uniform sampler3D image_cache;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform ivec3 image_block_size = ivec3(32, 32, 32);

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
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_entry_eye_coord;
uniform sampler2D ray_exit_tex_coord;
uniform sampler2D ray_exit_eye_coord;

uniform sampler1D transfer_function;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

vec4 compositeDVR(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth > 0.0)
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
  if (rayDepth > 0.0)
    rayDepth = currentRayLength;
  return curResult + color;
}

#define UNMAPPED 0
#define EMPTY 40000

void main()
{
#if GLSL_VERSION >= 130
  vec4 entryTexCoordAndZ = texelFetch(ray_entry_tex_coord, ivec2(gl_FragCoord.xy), 0);
  vec4 exitTexCoordAndZ = texelFetch(ray_exit_tex_coord, ivec2(gl_FragCoord.xy), 0);
#else
  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;
  vec4 entryTexCoordAndZ = texture2D(ray_entry_tex_coord, texCoords);
  vec4 exitTexCoordAndZ = texture2D(ray_exit_tex_coord, texCoords);
#endif
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    vec4 result = vec4(0.0);

#ifdef MIP
    float ch1V = 0.0;
#endif

    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
    float zeFront = texelFetch(ray_entry_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
    float zeBack = texelFetch(ray_exit_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
#else
    float zeFront = texture2D(ray_entry_eye_coord, texCoords).z;
    float zeBack = texture2D(ray_exit_eye_coord, texCoords).z;
#endif
    float ze = zeFront;
    float finalZe = 1.0;
    int curLevel = 0;
    float zeLength = zeBack - zeFront;
    float zeLengthRCP = 1.0 / zeLength;

    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * image_dimensions[curLevel]);
    float numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
    float stepSize = zeLength / (sampling_rate * numVoxel);

    bool finished = false;

    ivec3 pageDirAddress = ivec3(-1,-1,-1);
    ivec4 pageDirEntry = ivec4(-1,-1,-1,-1);
    ivec3 pageTableAddress = ivec3(-1,-1,-1);
    ivec4 pageTableEntry = ivec4(-1,-1,-1,-1);

    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = ze * ze_to_screen_pixel_voxel_size;
        while (voxel_world_sizes[curLevel] <= desiredVoxelSize && curLevel + 1 < LEVEL_COUNT) {
          ++curLevel;
          numVoxels = abs(rayVector * image_dimensions[curLevel]);
          numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
          stepSize = zeLength / (sampling_rate * numVoxel);
        }
        vec3 samplePos = startRayPosition + (ze - zeFront) * zeLengthRCP * rayVector;

        ivec3 voxelCoord = ivec3(samplePos * image_dimensions[curLevel]);
        ivec3 pageTableCoord = voxelCoord / image_block_size;
        ivec3 pageDirectoryCoord = pageTableCoord / page_table_block_size;
        ivec3 curPageDirAddress = page_directory_bases[curLevel] + pageDirectoryCoord;
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
          pageDirEntry = texelFetch(page_directory, pageDirAddress, 0);
        }
        int pagingFlag = pageDirEntry.w;
        if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
          ivec3 curPageTableAddress = pageDirEntry.xyz + pageTableCoord % page_table_block_size;
          if (curPageTableAddress != pageTableAddress) {
            pageTableAddress = curPageTableAddress;
            pageTableEntry = texelFetch(page_table_cache, pageTableAddress, 0);
          }
          pagingFlag = pageTableEntry.w;
          if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
            ivec3 voxelAddress = pageTableEntry.xyz + voxelCoord % image_block_size;
            float voxel = texelFetch(image_cache, voxelAddress, 0).r;

#ifdef MIP
#ifdef LOCAL_MIP
            if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
              finished = true;
            } else if (voxel > ch1V) {
              ch1V = voxel;
              finalZe = ze;
            }
#else
            if (voxel > ch1V) {
              ch1V = voxel;
              finalZe = ze;
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
              result = COMPOSITING(result, color, ze, finalZe);
              if (result.a >= 1.0) {
                result.a = 1.0;
                finished = true;
              }
            }
#endif // MIP
            ze += stepSize;
          } else {
            // skip empty space page table entry recursive
            if (pagingFlag == UNMAPPED) {
              vec3 testSamplePos;
              do {
                ze += stepSize;
                testSamplePos = startRayPosition + (ze - zeFront) * zeLengthRCP * rayVector;
              } while (ivec3(testSamplePos * image_dimensions[curLevel]) / image_block_size == pageTableCoord && ze > zeBack);
            } else { // empty block
              int nextNonEmptyLevel = curLevel + 1;
              int testPagingFlag = EMPTY;
              while (testPagingFlag == EMPTY && nextNonEmptyLevel < LEVEL_COUNT) {
                ivec3 testVoxelCoord = ivec3(samplePos * image_dimensions[nextNonEmptyLevel]);
                ivec3 testPageTableCoord = testVoxelCoord / image_block_size;
                ivec3 testPageDirectoryCoord = testPageTableCoord / page_table_block_size;

                ivec4 testPageDirEntry = texelFetch(page_directory, page_directory_bases[nextNonEmptyLevel] + testPageDirectoryCoord, 0);
                testPagingFlag = testPageDirEntry.w;
                if (testPagingFlag != UNMAPPED && pagingFlag != EMPTY) {
                  testPagingFlag = texelFetch(page_table_cache, testPageDirEntry.xyz + testPageTableCoord % page_table_block_size, 0).w;
                }
                ++nextNonEmptyLevel;
              }

              ivec3 prevBlock = ivec3(samplePos * image_dimensions[nextNonEmptyLevel-1]) / image_block_size;
              float testStepSize = -voxel_world_sizes[nextNonEmptyLevel-1] / sampling_rate;
              vec3 testSamplePos;
              do {
                ze += testStepSize;
                testSamplePos = startRayPosition + (ze - zeFront) * zeLengthRCP * rayVector;
              } while (ivec3(testSamplePos * image_dimensions[nextNonEmptyLevel-1]) / image_block_size == prevBlock && ze > zeBack);
            }
          }
        } else {
          // skip empty space page directory entry
          vec3 testSamplePos;
          do {
            ze += stepSize;
            testSamplePos = startRayPosition + (ze - zeFront) * zeLengthRCP * rayVector;
          } while (ivec3(testSamplePos * image_dimensions[curLevel]) / image_block_size == pageTableCoord && ze > zeBack);
        }

        finished = finished || (ze <= zeBack);
      } // for
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

    if (finalZe < 0.0) {
      gl_FragDepth = ze_to_zw_a / finalZe + ze_to_zw_b;
      gl_FragDepth = 0.3;
    } else {
#ifdef RESULT_OPAQUE
      gl_FragDepth = entryTexCoordAndZ.w;
#else
      gl_FragDepth = 1.0;
#endif
    }

    result.rgb *= result.a;
    FragData0 = result;
  }
}

