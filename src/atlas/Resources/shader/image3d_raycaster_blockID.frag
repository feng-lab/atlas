#if GLSL_VERSION < 130
#extension GL_EXT_gpu_shader4 : enable
#define uint unsigned int
#endif

uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform uvec4 pos_to_block_ids[LEVEL_COUNT];

// uniform vec2 screen_dim_RCP;
uniform float sampling_rate;
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_entry_eye_coord;
uniform sampler2D ray_exit_tex_coord;
uniform sampler2D ray_exit_eye_coord;

uniform sampler2D last_ray_depth;

#if GLSL_VERSION >= 330
layout(location = 0) out uvec4 FragData0;
layout(location = 1) out uvec4 FragData1;
layout(location = 2) out uvec4 FragData2;
layout(location = 3) out uvec4 FragData3;
layout(location = 4) out uvec4 FragData4;
layout(location = 5) out uvec4 FragData5;
layout(location = 6) out uvec4 FragData6;
layout(location = 7) out uvec4 FragData7;
#elif GLSL_VERSION >= 130
out uvec4 FragData0;  // call glBindFragDataLocation before linking
out uvec4 FragData1;  // call glBindFragDataLocation before linking
out uvec4 FragData2;  // call glBindFragDataLocation before linking
out uvec4 FragData3;  // call glBindFragDataLocation before linking
out uvec4 FragData4;  // call glBindFragDataLocation before linking
out uvec4 FragData5;  // call glBindFragDataLocation before linking
out uvec4 FragData6;  // call glBindFragDataLocation before linking
out uvec4 FragData7;  // call glBindFragDataLocation before linking
#else
varying out uvec4 FragData0;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData1;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData2;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData3;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData4;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData5;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData6;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData7;  // call glBindFragDataLocationForce before linking
#endif

#define UNMAPPED 0
#define EMPTY 40000
#define UINTMAX 4294967295U

void main()
{
#if GLSL_VERSION >= 130
  float currentRayLength = texelFetch(last_ray_depth, ivec2(gl_FragCoord.xy), 0).x;
#else
  float currentRayLength = texelFetch2D(last_ray_depth, ivec2(gl_FragCoord.xy), 0).x;
#endif
  if (currentRayLength >= 1.0) {
    discard;
  }

//  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;
//#if GLSL_VERSION >= 130
//  vec4 entryTexCoordAndZ = texture(ray_entry_tex_coord, texCoords);
//  vec4 exitTexCoordAndZ = texture(ray_exit_tex_coord, texCoords);
//#else
//  vec4 entryTexCoordAndZ = texture2D(ray_entry_tex_coord, texCoords);
//  vec4 exitTexCoordAndZ = texture2D(ray_exit_tex_coord, texCoords);
//#endif
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
    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
//#if GLSL_VERSION >= 130
//    float zeFront = texture(ray_entry_eye_coord, texCoords).z;
//    float zeBack = texture(ray_exit_eye_coord, texCoords).z;
//#else
//    float zeFront = texture2D(ray_entry_eye_coord, texCoords).z;
//    float zeBack = texture2D(ray_exit_eye_coord, texCoords).z;
//#endif
#if GLSL_VERSION >= 130
    float zeFront = texelFetch(ray_entry_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
    float zeBack = texelFetch(ray_exit_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
#else
    float zeFront = texelFetch2D(ray_entry_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
    float zeBack = texelFetch2D(ray_exit_eye_coord, ivec2(gl_FragCoord.xy), 0).z;
#endif
    int curLevel = 0;

//    uint missBlockIDs[16] = uint[16](0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u);
//    int missBlockIDsIndex = 0;
//    uint usedBlockIDs[16] = uint[16](0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u);
//    int usedBlockIDsIndex = 0;
    uint missBlockIDs[32] = uint[32](0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u,
                                     0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u);
    int missBlockIDsIndex = 0;

    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * image_dimensions[curLevel]);
    float stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

    // float currentRayLength = 0.0;
    bool finished = false;

    uvec3 pageDirAddress = uvec3(UINTMAX,UINTMAX,UINTMAX);
    uvec4 pageDirEntry = uvec4(UINTMAX,UINTMAX,UINTMAX,UINTMAX);

    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * ze_to_screen_pixel_voxel_size;
        while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
          ++curLevel;
          numVoxels = abs(rayVector * image_dimensions[curLevel]);
          stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
        }
        if (curLevel + 1 == LEVEL_COUNT) {
          missBlockIDs[missBlockIDsIndex++] = UINTMAX;
          finished = true;
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
            // save used blockid
            if (missBlockIDsIndex < 32) {
              uint blockID = pos_to_block_ids[curLevel].w + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
              missBlockIDs[missBlockIDsIndex++] = blockID;
              finished = missBlockIDsIndex == 32;
            }

            // goto next block
            do {
              currentRayLength += stepSize;
            } while (uvec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength < 1.0);
          } else {
            if (pagingFlag == UNMAPPED) {
              // save missed blockid
              if (missBlockIDsIndex < 32) {
                uint blockID = pos_to_block_ids[curLevel].w + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
                missBlockIDs[missBlockIDsIndex++] = blockID;
                finished = missBlockIDsIndex == 32;
              }

              // goto next block
              do {
                currentRayLength += stepSize;
              } while (uvec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength < 1.0);
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
            // save missed blockid
            if (missBlockIDsIndex < 32) {
              uint blockID = pos_to_block_ids[curLevel].w + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
              missBlockIDs[missBlockIDsIndex++] = blockID;
              finished = missBlockIDsIndex == 32;
            }

            // goto next block
            do {
              currentRayLength += stepSize;
            } while (uvec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength < 1.0);
          }
        }

        finished = finished || (currentRayLength >= 1.0);
      } // for
    }

    FragData0 = uvec4(missBlockIDs[0], missBlockIDs[1], missBlockIDs[2], missBlockIDs[3]);
    FragData1 = uvec4(missBlockIDs[4], missBlockIDs[5], missBlockIDs[6], missBlockIDs[7]);
    FragData2 = uvec4(missBlockIDs[8], missBlockIDs[9], missBlockIDs[10], missBlockIDs[11]);
    FragData3 = uvec4(missBlockIDs[12], missBlockIDs[13], missBlockIDs[14], missBlockIDs[15]);
    FragData4 = uvec4(missBlockIDs[16], missBlockIDs[17], missBlockIDs[18], missBlockIDs[19]);
    FragData5 = uvec4(missBlockIDs[20], missBlockIDs[21], missBlockIDs[22], missBlockIDs[23]);
    FragData6 = uvec4(missBlockIDs[24], missBlockIDs[25], missBlockIDs[26], missBlockIDs[27]);
    FragData7 = uvec4(missBlockIDs[28], missBlockIDs[29], missBlockIDs[30], missBlockIDs[31]);
//    FragData4 = uvec4(usedBlockIDs[0], usedBlockIDs[1], usedBlockIDs[2], usedBlockIDs[3]);
//    FragData5 = uvec4(usedBlockIDs[4], usedBlockIDs[5], usedBlockIDs[6], usedBlockIDs[7]);
//    FragData6 = uvec4(usedBlockIDs[8], usedBlockIDs[9], usedBlockIDs[10], usedBlockIDs[11]);
//    FragData7 = uvec4(usedBlockIDs[12], usedBlockIDs[13], usedBlockIDs[14], usedBlockIDs[15]);
  }
}

