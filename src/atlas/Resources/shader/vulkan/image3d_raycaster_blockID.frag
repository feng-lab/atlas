#version 450

// Block ID capture ray pass

layout(location = 0) out uvec4 FragData0;
layout(location = 1) out uvec4 FragData1;
layout(location = 2) out uvec4 FragData2;
layout(location = 3) out uvec4 FragData3;
layout(location = 4) out uvec4 FragData4;
layout(location = 5) out uvec4 FragData5;
layout(location = 6) out uvec4 FragData6;
layout(location = 7) out uvec4 FragData7;

#include "include/raycaster_common.glslinc"

void main()
{
  vec4 entryTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 0), 0);
  vec4 exitTexCoordAndZ  = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 1), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition  = exitTexCoordAndZ.xyz;
  if (all(equal(startRayPosition, exitRayPosition))) discard;

  float zeFront = entryTexCoordAndZ.w;
  float zeBack  = exitTexCoordAndZ.w;
  int curLevel = 0;

  vec3 rayVector = exitRayPosition - startRayPosition;
  vec3 numVoxels = abs(rayVector * pg.image_dimensions[curLevel]);
  float stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

  uint missBlockIDs[32];
  int missBlockIDsIndex = 0;
  for (int i = 0; i < 32; ++i) missBlockIDs[i] = 0u;

  uvec3 pageDirAddress = uvec3(0xFFFFFFFFu);
  uvec4 pageDirEntry   = uvec4(0xFFFFFFFFu);

  float currentRayLength = 0.0;
  for (int loop0=0; missBlockIDsIndex < 32 && loop0<255; ++loop0) {
    for (int loop1=0; missBlockIDsIndex < 32 && loop1<255; ++loop1) {
      float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * pg.ze_to_screen_pixel_voxel_size;
      while (curLevel + 1 < LEVEL_COUNT && pg.voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
        ++curLevel;
        numVoxels = abs(rayVector * pg.image_dimensions[curLevel]);
        stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
      }
      if (curLevel + 1 == LEVEL_COUNT) {
        missBlockIDs[missBlockIDsIndex++] = 0xFFFFFFFFu; // sentinel
        break;
      }

      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
      uvec3 voxelCoord = clamp(uvec3(samplePos * pg.image_dimensions[curLevel]), uvec3(0u), pg.image_dimensions[curLevel] - 1u);
      uvec3 pageTableCoord = voxelCoord / pg.image_block_size;
      uvec3 curPageDirAddress = pg.page_directory_bases[curLevel] + pageTableCoord / pg.page_table_block_size;
      if (curPageDirAddress != pageDirAddress) {
        pageDirAddress = curPageDirAddress;
        pageDirEntry = texelFetch(page_directory, ivec3(pageDirAddress), 0);
      }
      uint pagingFlag = pageDirEntry.w;
      if (pagingFlag != 0u && pagingFlag != 40000u) {
        uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % pg.page_table_block_size), 0);
        if (pageTableEntry.w != 40000u) {
          // record block id
          if (missBlockIDsIndex < 32) {
            uint blockID = pg.pos_to_block_ids[curLevel].x + pageTableCoord.x
                         + pg.pos_to_block_ids[curLevel].y * pageTableCoord.y
                         + pg.pos_to_block_ids[curLevel].z * pageTableCoord.z;
            missBlockIDs[missBlockIDsIndex++] = blockID;
          }
        }
        // advance to next block
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.image_dimensions[curLevel]), uvec3(0u), pg.image_dimensions[curLevel] - 1u);
        } while (voxelCoord / pg.image_block_size == pageTableCoord && currentRayLength <= 1.0);
      } else if (pagingFlag == 0u) {
        // unmapped page directory: record block id
        if (missBlockIDsIndex < 32) {
          uint blockID = pg.pos_to_block_ids[curLevel].x + pageTableCoord.x
                       + pg.pos_to_block_ids[curLevel].y * pageTableCoord.y
                       + pg.pos_to_block_ids[curLevel].z * pageTableCoord.z;
          missBlockIDs[missBlockIDsIndex++] = blockID;
        }
        // advance to next block
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.image_dimensions[curLevel]), uvec3(0u), pg.image_dimensions[curLevel] - 1u);
        } while (voxelCoord / pg.image_block_size == pageTableCoord && currentRayLength <= 1.0);
      } else { // EMPTY page directory
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.image_dimensions[curLevel]), uvec3(0u), pg.image_dimensions[curLevel] - 1u);
        } while (pg.page_directory_bases[curLevel] + voxelCoord / pg.image_block_size / pg.page_table_block_size == pageDirAddress && currentRayLength <= 1.0);
      }
    }
  }

  // Write 32 IDs across 8 uvec4 attachments
  uvec4 outv[8];
  for (int i=0;i<8;++i) {
    int b = i*4;
    outv[i] = uvec4(missBlockIDs[b+0], missBlockIDs[b+1], missBlockIDs[b+2], missBlockIDs[b+3]);
  }
  FragData0 = outv[0];
  FragData1 = outv[1];
  FragData2 = outv[2];
  FragData3 = outv[3];
  FragData4 = outv[4];
  FragData5 = outv[5];
  FragData6 = outv[6];
  FragData7 = outv[7];
}
