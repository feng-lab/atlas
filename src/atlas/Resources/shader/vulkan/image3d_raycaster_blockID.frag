#version 450
#extension GL_GOOGLE_include_directive : require

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
// Match GL shader constants
const uint UINTMAX = 0xFFFFFFFFu;

// Set to 1 to dump diagnostic values instead of block IDs.
// Writes debug floats (bit-cast to uint) into attachments and returns.
#ifndef BLOCKID_DEBUG_ZE_DUMP
#define BLOCKID_DEBUG_ZE_DUMP 0
#endif

void main()
{
  // Parity with GL: initialize currentRayLength from last ray depth and discard if already complete
  float currentRayLength =
    texelFetch(atlas_bindlessSampler2DNearest(rp.last_ray_depth_tex), ivec2(gl_FragCoord.xy), 0).x;
  if (currentRayLength >= 1.0) { discard; }

  vec4 entryTexCoordAndZ =
    texelFetch(atlas_bindlessSampler2DArrayNearest(rp.ray_entry_exit_tex_coord), ivec3(gl_FragCoord.xy, 0), 0);
  vec4 exitTexCoordAndZ =
    texelFetch(atlas_bindlessSampler2DArrayNearest(rp.ray_entry_exit_tex_coord), ivec3(gl_FragCoord.xy, 1), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition  = exitTexCoordAndZ.xyz;
  if (all(equal(startRayPosition, exitRayPosition))) discard;

  float zeFront = entryTexCoordAndZ.w;
  float zeBack  = exitTexCoordAndZ.w;
  int curLevel = 0;

#if BLOCKID_DEBUG_ZE_DUMP
  // Debug: emit ze_to_screen_pixel_voxel_size, zeFront, and two highest-level voxel sizes.
  // Use safe indices in case LEVEL_COUNT < 5.
  int i3 = min(3, LEVEL_COUNT - 1);
  int i4 = min(4, LEVEL_COUNT - 1);
  FragData0 = uvec4(floatBitsToUint(pg.ze_to_screen_pixel_voxel_size),
                    floatBitsToUint(zeFront),
                    0u,
                    0u);
  FragData1 = uvec4(floatBitsToUint(pg.levels[i3].voxel_world_size_pad.x),
                    floatBitsToUint(pg.levels[i4].voxel_world_size_pad.x),
                    0u,
                    0u);
  FragData2 = uvec4(0u);
  FragData3 = uvec4(0u);
  FragData4 = uvec4(0u);
  FragData5 = uvec4(0u);
  FragData6 = uvec4(0u);
  FragData7 = uvec4(0u);
  return;
#endif

  vec3 rayVector = exitRayPosition - startRayPosition;
  vec3 numVoxels = abs(rayVector * pg.levels[curLevel].image_dimensions.xyz);
  float stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

  uint missBlockIDs[32];
  int missBlockIDsIndex = 0;
  for (int i = 0; i < 32; ++i) missBlockIDs[i] = 0u;

  uvec3 pageDirAddress = uvec3(0xFFFFFFFFu);
  uvec4 pageDirEntry   = uvec4(0xFFFFFFFFu);

  // currentRayLength initialized from last_ray_depth_tex above
  bool finished = false;
  for (int loop0=0; !finished && loop0<255; ++loop0) {
    for (int loop1=0; !finished && loop1<255; ++loop1) {
      float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * pg.ze_to_screen_pixel_voxel_size;
      while (curLevel + 1 < LEVEL_COUNT && pg.levels[curLevel+1].voxel_world_size_pad.x <= desiredVoxelSize) {
        ++curLevel;
        numVoxels = abs(rayVector * pg.levels[curLevel].image_dimensions.xyz);
        stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
      }
      if (curLevel + 1 == LEVEL_COUNT) {
        missBlockIDs[missBlockIDsIndex++] = UINTMAX;
        finished = true;
        break;
      }

      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
      uvec3 voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
      uvec3 pageTableCoord = voxelCoord / pg.image_block_size.xyz;
      uvec3 curPageDirAddress = pg.levels[curLevel].page_directory_base.xyz + pageTableCoord / pg.page_table_block_size.xyz;
      if (curPageDirAddress != pageDirAddress) {
        pageDirAddress = curPageDirAddress;
        pageDirEntry =
          texelFetch(atlas_bindlessUSampler3DNearest(rp.page_directory), ivec3(pageDirAddress), 0);
      }
      uint pagingFlag = pageDirEntry.w;
      if (pagingFlag != 0u && pagingFlag != 40000u) {
        uvec4 pageTableEntry =
          texelFetch(atlas_bindlessUSampler3DNearest(rp.page_table_cache),
                     ivec3(pageDirEntry.xyz + (pageTableCoord % pg.page_table_block_size.xyz)),
                     0);
        if (pageTableEntry.w != 40000u) {
          // record block id
          if (missBlockIDsIndex < 32) {
            uint blockID = pg.levels[curLevel].pos_to_block_ids.x + pageTableCoord.x
                         + pg.levels[curLevel].pos_to_block_ids.y * pageTableCoord.y
                         + pg.levels[curLevel].pos_to_block_ids.z * pageTableCoord.z;
            missBlockIDs[missBlockIDsIndex++] = blockID;
            if (missBlockIDsIndex == 32) { finished = true; }
          }
        }
        // advance to next block
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
        } while (voxelCoord / pg.image_block_size.xyz == pageTableCoord && currentRayLength <= 1.0);
      } else if (pagingFlag == 0u) {
        // unmapped page directory: record block id
        if (missBlockIDsIndex < 32) {
          uint blockID = pg.levels[curLevel].pos_to_block_ids.x + pageTableCoord.x
                       + pg.levels[curLevel].pos_to_block_ids.y * pageTableCoord.y
                       + pg.levels[curLevel].pos_to_block_ids.z * pageTableCoord.z;
          missBlockIDs[missBlockIDsIndex++] = blockID;
          if (missBlockIDsIndex == 32) { finished = true; }
        }
        // advance to next block
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
        } while (voxelCoord / pg.image_block_size.xyz == pageTableCoord && currentRayLength <= 1.0);
      } else { // EMPTY page directory
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
        } while (pg.levels[curLevel].page_directory_base.xyz + (voxelCoord / pg.image_block_size.xyz) / pg.page_table_block_size.xyz == pageDirAddress && currentRayLength <= 1.0);
      }
      // Match GL: terminate when the ray marches past 1.0
      finished = finished || (currentRayLength > 1.0);
    }
  }

  // Write 32 IDs across 8 uvec4 attachments
#if 0
  // Debug mode: write a known constant per pixel to FragData0 to verify draw
  uvec4 outv[8];
  for (int i=0;i<8;++i) outv[i] = uvec4(123u);
  FragData0 = outv[0];
  FragData1 = outv[1];
  FragData2 = outv[2];
  FragData3 = outv[3];
  FragData4 = outv[4];
  FragData5 = outv[5];
  FragData6 = outv[6];
  FragData7 = outv[7];
#else
  FragData0 = uvec4(missBlockIDs[0], missBlockIDs[1], missBlockIDs[2], missBlockIDs[3]);
  // FragData0 = uvec4(LEVEL_COUNT, uint(curLevel), pageDirEntry.w, 0); 
  FragData1 = uvec4(missBlockIDs[4], missBlockIDs[5], missBlockIDs[6], missBlockIDs[7]);
  FragData2 = uvec4(missBlockIDs[8], missBlockIDs[9], missBlockIDs[10], missBlockIDs[11]);
  FragData3 = uvec4(missBlockIDs[12], missBlockIDs[13], missBlockIDs[14], missBlockIDs[15]);
  FragData4 = uvec4(missBlockIDs[16], missBlockIDs[17], missBlockIDs[18], missBlockIDs[19]);
  FragData5 = uvec4(missBlockIDs[20], missBlockIDs[21], missBlockIDs[22], missBlockIDs[23]);
  FragData6 = uvec4(missBlockIDs[24], missBlockIDs[25], missBlockIDs[26], missBlockIDs[27]);
  FragData7 = uvec4(missBlockIDs[28], missBlockIDs[29], missBlockIDs[30], missBlockIDs[31]);
#endif
}
