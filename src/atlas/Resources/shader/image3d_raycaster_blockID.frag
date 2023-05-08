uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform uvec3 pos_to_block_ids[LEVEL_COUNT];

// uniform vec2 screen_dim_RCP;
uniform float sampling_rate;
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2DArray ray_entry_exit_tex_coord;

uniform sampler2D last_ray_depth;

layout(location = 0) out uvec4 FragData0;
layout(location = 1) out uvec4 FragData1;
layout(location = 2) out uvec4 FragData2;
layout(location = 3) out uvec4 FragData3;
layout(location = 4) out uvec4 FragData4;
layout(location = 5) out uvec4 FragData5;
layout(location = 6) out uvec4 FragData6;
layout(location = 7) out uvec4 FragData7;

#define UNMAPPED 0
#define EMPTY 40000
#define UINTMAX 4294967295U

void main()
{
  float currentRayLength = texelFetch(last_ray_depth, ivec2(gl_FragCoord.xy), 0).x;
  if (currentRayLength >= 1.0) {
    discard;
  }

  vec4 entryTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 0), 0);
  vec4 exitTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 1), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
    float zeFront = entryTexCoordAndZ.w;
    float zeBack = exitTexCoordAndZ.w;
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
        vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
        uvec3 voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);

        uvec3 pageTableCoord = voxelCoord / image_block_size;
        uvec3 curPageDirAddress = page_directory_bases[curLevel] + pageTableCoord / page_table_block_size;
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
          pageDirEntry = texelFetch(page_directory, ivec3(pageDirAddress), 0);
        }
        uint pagingFlag = pageDirEntry.w;
        if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
          uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
          pagingFlag = pageTableEntry.w;
          if (pagingFlag != EMPTY) { // unmapped or (mapped and not empty)
            // save used or missed blockid
            if (missBlockIDsIndex < 32) {
              uint blockID = pos_to_block_ids[curLevel].x + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
              missBlockIDs[missBlockIDsIndex++] = blockID;
              finished = missBlockIDsIndex == 32;
            }
          }

          // goto next block
          do {
            currentRayLength += stepSize;
            samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
            voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
          } while (voxelCoord / image_block_size == pageTableCoord && currentRayLength <= 1.0);
        } else {
          if (pagingFlag == EMPTY) {
            do { // skip empty space page directory entry
              currentRayLength += stepSize;
              samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
              voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
            } while (page_directory_bases[curLevel] + voxelCoord / image_block_size / page_table_block_size == pageDirAddress && currentRayLength <= 1.0);
          } else { // pagingFlag == UNMAPPED
            // save missed blockid
            if (missBlockIDsIndex < 32) {
              uint blockID = pos_to_block_ids[curLevel].x + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
              missBlockIDs[missBlockIDsIndex++] = blockID;
              finished = missBlockIDsIndex == 32;
            }

            // goto next block
            do {
              currentRayLength += stepSize;
              samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
              voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
            } while (voxelCoord / image_block_size == pageTableCoord && currentRayLength <= 1.0);
          }
        }

        finished = finished || (currentRayLength > 1.0);
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

