#if GLSL_VERSION < 130
#extension GL_EXT_gpu_shader4 : enable
#define uint unsigned int
#endif

uniform isampler3D page_directory;
uniform ivec3 page_directory_bases[LEVEL_COUNT];
uniform isampler3D page_table_cache;
uniform ivec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform ivec3 image_block_size;
uniform uvec4 pos_to_block_ids[LEVEL_COUNT];

uniform vec2 screen_dim_RCP;
uniform float sampling_rate;
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_entry_eye_coord;
uniform sampler2D ray_exit_tex_coord;
uniform sampler2D ray_exit_eye_coord;

#if GLSL_VERSION >= 330
layout(location = 0) out uvec4 FragData0;
layout(location = 1) out uvec4 FragData1;
layout(location = 2) out uvec4 FragData2;
layout(location = 3) out uvec4 FragData3;
layout(location = 4) out uvec4 FragData4;
#elif GLSL_VERSION >= 130
out uvec4 FragData0;  // call glBindFragDataLocation before linking
out uvec4 FragData1;  // call glBindFragDataLocation before linking
out uvec4 FragData2;  // call glBindFragDataLocation before linking
out uvec4 FragData3;  // call glBindFragDataLocation before linking
out uvec4 FragData4;  // call glBindFragDataLocation before linking
#else
varying out uvec4 FragData0;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData1;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData2;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData3;  // call glBindFragDataLocationForce before linking
varying out uvec4 FragData4;  // call glBindFragDataLocationForce before linking
#endif

#define UNMAPPED 0
#define EMPTY 40000

void main()
{
  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;
#if GLSL_VERSION >= 130
  vec4 entryTexCoordAndZ = texture(ray_entry_tex_coord, texCoords);
  vec4 exitTexCoordAndZ = texture(ray_exit_tex_coord, texCoords);
#else
  vec4 entryTexCoordAndZ = texture2D(ray_entry_tex_coord, texCoords);
  vec4 exitTexCoordAndZ = texture2D(ray_exit_tex_coord, texCoords);
#endif
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
    float zeFront = texture(ray_entry_eye_coord, texCoords).z;
    float zeBack = texture(ray_exit_eye_coord, texCoords).z;
#else
    float zeFront = texture2D(ray_entry_eye_coord, texCoords).z;
    float zeBack = texture2D(ray_exit_eye_coord, texCoords).z;
#endif
    int curLevel = 0;

    uint missBlockIDs[8] = uint[8](0u,0u,0u,0u, 0u,0u,0u,0u);
    int missBlockIDsIndex = 0;
    uint usedBlockIDs[12] = uint[12](0u,0u,0u,0u, 0u,0u,0u,0u, 0u,0u,0u,0u);
    int usedBlockIDsIndex = 0;

    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * image_dimensions[curLevel]);
    float stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

    float currentRayLength = 0.0;
    bool finished = false;

    ivec3 pageDirAddress = ivec3(-1,-1,-1);
    ivec4 pageDirEntry = ivec4(-1,-1,-1,-1);
    ivec3 pageTableAddress = ivec3(-1,-1,-1);
    ivec4 pageTableEntry = ivec4(-1,-1,-1,-1);

    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * ze_to_screen_pixel_voxel_size;
        while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
          ++curLevel;
          numVoxels = abs(rayVector * image_dimensions[curLevel]);
          stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
        }
        vec3 samplePos = startRayPosition + currentRayLength * rayVector;

        ivec3 voxelCoord = ivec3(samplePos * image_dimensions[curLevel]);
        ivec3 pageTableCoord = voxelCoord / image_block_size;
        ivec3 curPageDirAddress = page_directory_bases[curLevel] + pageTableCoord / page_table_block_size;
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
#if GLSL_VERSION >= 130
          pageDirEntry = texelFetch(page_directory, pageDirAddress, 0);
#else
          pageDirEntry = texelFetch3D(page_directory, pageDirAddress, 0);
#endif
        }
        int pagingFlag = pageDirEntry.w;
        if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
          ivec3 curPageTableAddress = pageDirEntry.xyz + pageTableCoord % page_table_block_size;
          if (curPageTableAddress != pageTableAddress) {
            pageTableAddress = curPageTableAddress;
#if GLSL_VERSION >= 130
            pageTableEntry = texelFetch(page_table_cache, pageTableAddress, 0);
#else
            pageTableEntry = texelFetch3D(page_table_cache, pageTableAddress, 0);
#endif
          }
          pagingFlag = pageTableEntry.w;
          if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
            // save used blockid
            if (usedBlockIDsIndex < 12) {
              uint blockID = pos_to_block_ids[curLevel].w + uint(pageTableCoord.x) + pos_to_block_ids[curLevel].y * uint(pageTableCoord.y) + pos_to_block_ids[curLevel].z * uint(pageTableCoord.z);
              usedBlockIDs[usedBlockIDsIndex++] = blockID;
            }

            // goto next block
            do {
              currentRayLength += stepSize;
            } while (ivec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength <= 1.0);
          } else {
            // skip empty space page table entry recursive
            if (pagingFlag == UNMAPPED) {
              do {
                currentRayLength += stepSize;
              } while (ivec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength <= 1.0);
            } else { // empty block
              int nextNonEmptyLevel = curLevel + 1;
              int testPagingFlag = EMPTY;
              while (testPagingFlag == EMPTY && nextNonEmptyLevel < LEVEL_COUNT) {
                ivec3 testVoxelCoord = ivec3(samplePos * image_dimensions[nextNonEmptyLevel]);
                ivec3 testPageTableCoord = testVoxelCoord / image_block_size;

#if GLSL_VERSION >= 130
                ivec4 testPageDirEntry = texelFetch(page_directory, page_directory_bases[nextNonEmptyLevel] + testPageTableCoord / page_table_block_size, 0);
#else
                ivec4 testPageDirEntry = texelFetch3D(page_directory, page_directory_bases[nextNonEmptyLevel] + testPageTableCoord / page_table_block_size, 0);
#endif
                testPagingFlag = testPageDirEntry.w;
                if (testPagingFlag != UNMAPPED && testPagingFlag != EMPTY) {
#if GLSL_VERSION >= 130
                  testPagingFlag = texelFetch(page_table_cache, testPageDirEntry.xyz + testPageTableCoord % page_table_block_size, 0).w;
#else
                  testPagingFlag = texelFetch3D(page_table_cache, testPageDirEntry.xyz + testPageTableCoord % page_table_block_size, 0).w;
#endif
                }
                ++nextNonEmptyLevel;
              }

              ivec3 prevBlock = ivec3(samplePos * image_dimensions[nextNonEmptyLevel-1]) / image_block_size;
              numVoxels = abs(rayVector * image_dimensions[nextNonEmptyLevel-1]);
              float testStepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
              do {
                currentRayLength += testStepSize;
              } while (ivec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[nextNonEmptyLevel-1]) / image_block_size == prevBlock && currentRayLength <= 1.0);
            }
          }
        } else {
          if (pagingFlag == EMPTY) {
            do { // skip empty space page directory entry
              currentRayLength += stepSize;
            } while (page_directory_bases[curLevel] + ivec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size / page_table_block_size == pageDirAddress && currentRayLength <= 1.0);
          } else { // pagingFlag == UNMAPPED
            do { // skip empty space page table entry
              currentRayLength += stepSize;
            } while (ivec3((startRayPosition + currentRayLength * rayVector) * image_dimensions[curLevel]) / image_block_size == pageTableCoord && currentRayLength <= 1.0);
          }
        }

        // save missed blockid
        if (pagingFlag == UNMAPPED && missBlockIDsIndex < 8) {
          uint blockID = pos_to_block_ids[curLevel].w + uint(pageTableCoord.x) + pos_to_block_ids[curLevel].y * uint(pageTableCoord.y) + pos_to_block_ids[curLevel].z * uint(pageTableCoord.z);
          missBlockIDs[missBlockIDsIndex++] = blockID;
          finished = missBlockIDsIndex == 8;
        }

        finished = finished || (currentRayLength > 1.0);
      } // for
    }

    FragData0 = uvec4(missBlockIDs[0], missBlockIDs[1], missBlockIDs[2], missBlockIDs[3]);
    FragData1 = uvec4(missBlockIDs[4], missBlockIDs[5], missBlockIDs[6], missBlockIDs[7]);
    FragData2 = uvec4(usedBlockIDs[0], usedBlockIDs[1], usedBlockIDs[2], usedBlockIDs[3]);
    FragData3 = uvec4(usedBlockIDs[4], usedBlockIDs[5], usedBlockIDs[6], usedBlockIDs[7]);
    FragData4 = uvec4(usedBlockIDs[8], usedBlockIDs[9], usedBlockIDs[10], usedBlockIDs[11]);
  }
}

