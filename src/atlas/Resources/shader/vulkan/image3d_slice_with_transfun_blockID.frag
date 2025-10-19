#version 450

// Outputs a block ID for the sampled slice position, based on paging.

layout(location = 0) out uvec4 FragData0;

#include "include/raycaster_common.glslinc"

layout(location = 0) in vec3 texCoord0;
layout(location = 1) in vec4 eyeCoord; // only .z used

void main()
{
  float desiredVoxelSize = eyeCoord.z * pg.ze_to_screen_pixel_voxel_size;
  int curLevel = 0;
  while (curLevel + 1 < LEVEL_COUNT && pg.levels[curLevel + 1].voxel_world_size_pad.x <= desiredVoxelSize) {
    ++curLevel;
  }

  if (curLevel + 1 == LEVEL_COUNT) {
    FragData0 = uvec4(0xFFFFFFFFu, 0u, 0u, 0u);
    return;
  }

  uvec3 voxelCoord = clamp(uvec3(texCoord0 * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
  uvec3 pageTableCoord = voxelCoord / pg.image_block_size.xyz;
  uint blockID = pg.levels[curLevel].pos_to_block_ids.x + pageTableCoord.x
               + pg.levels[curLevel].pos_to_block_ids.y * pageTableCoord.y
               + pg.levels[curLevel].pos_to_block_ids.z * pageTableCoord.z;
  if (curLevel + 1 < LEVEL_COUNT) {
    uint nextBase = pg.levels[curLevel + 1].pos_to_block_ids.x;
    if (blockID >= nextBase) { blockID = 0xFFFFFFFFu; }
  }

  uvec4 pageDirEntry = texelFetch(page_directory, ivec3(pg.levels[curLevel].page_directory_base.xyz + pageTableCoord / pg.page_table_block_size.xyz), 0);
  uint pagingFlag = pageDirEntry.w;
  const uint UNMAPPED = 0u;
  const uint EMPTY = 40000u;

  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + (pageTableCoord % pg.page_table_block_size.xyz)), 0);
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != EMPTY) {
      FragData0 = uvec4(blockID, 0u, 0u, 0u);
      return;
    }
  } else if (pagingFlag == UNMAPPED) {
    FragData0 = uvec4(blockID, 0u, 0u, 0u);
    return;
  }

  FragData0 = uvec4(0u, 0u, 0u, 0u);
}
