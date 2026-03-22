#version 450
#extension GL_GOOGLE_include_directive : require

// Outputs a block ID for the sampled slice position, based on paging.

layout(location = 0) out uvec4 FragData0;

// Number of paging levels (specialization constant)
layout(constant_id = 70) const int LEVEL_COUNT = 1;

#include "include/bindless.glslinc"

// Bindless texture indices. Keep these in a UBO instead of push constants so
// we don't overlap the vertex shader's 2xmat4 push-constant range.
layout(std140, set = 1, binding = 0) uniform SlicePagedBindlessUBO {
  uint page_directory;
  uint page_table_cache;
  uint image_cache;
  uint volume;
  uint colormap;
  uint _pad0;
  uint _pad1;
  uint _pad2;
} sbubo;

// Paging/geometry parameters (set 2)
struct PageLevelData {
  uvec4 page_directory_base;
  uvec4 image_dimensions;
  uvec4 pos_to_block_ids;
  vec4  voxel_world_size_pad; // .x used
};

layout(std140, set = 2, binding = 2) uniform PageData {
  uvec4 page_table_block_size;
  uvec4 image_block_size;
  vec4  image_address_to_normalized_texture_coord;
  float ze_to_screen_pixel_voxel_size;

  PageLevelData levels[LEVEL_COUNT];
} pg;

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

  uvec4 pageDirEntry = texelFetch(atlas_bindlessUSampler3DNearest(sbubo.page_directory),
                                  ivec3(pg.levels[curLevel].page_directory_base.xyz +
                                        pageTableCoord / pg.page_table_block_size.xyz),
                                  0);
  uint pagingFlag = pageDirEntry.w;
  const uint UNMAPPED = 0u;
  const uint EMPTY = 40000u;

  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    uvec4 pageTableEntry = texelFetch(atlas_bindlessUSampler3DNearest(sbubo.page_table_cache),
                                      ivec3(pageDirEntry.xyz + (pageTableCoord % pg.page_table_block_size.xyz)),
                                      0);
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
