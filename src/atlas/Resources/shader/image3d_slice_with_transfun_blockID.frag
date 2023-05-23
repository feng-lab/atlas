in vec3 texCoord0;
in vec4 eyeCoord;

uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform uvec3 pos_to_block_ids[LEVEL_COUNT];

uniform float ze_to_screen_pixel_voxel_size;

layout(location = 0) out uvec4 FragData0;

#define UNMAPPED 0U
#define EMPTY 40000U
#define UINTMAX 4294967295U

void main()
{
#if NUM_VOLUMES > 0
  float desiredVoxelSize = eyeCoord.z * ze_to_screen_pixel_voxel_size;
  int curLevel = 0;
  while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
    ++curLevel;
  }

  if (curLevel + 1 == LEVEL_COUNT) {
    FragData0 = uvec4(UINTMAX, 0, 0, 0);
    return;
  }

  uvec3 voxelCoord = clamp(uvec3(texCoord0 * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
  uvec3 pageTableCoord = voxelCoord / image_block_size;
  uint blockID = pos_to_block_ids[curLevel].x + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
  uvec4 pageDirEntry = texelFetch(page_directory, ivec3(page_directory_bases[curLevel] + pageTableCoord / page_table_block_size), 0);
  uint pagingFlag = pageDirEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != EMPTY) { // unmapped or (mapped and not empty)
      FragData0 = uvec4(blockID, 0, 0, 0);
      return;
    }
  } else if (pagingFlag == UNMAPPED) {
    FragData0 = uvec4(blockID, 0, 0, 0);
    return;
  }
  FragData0 = uvec4(0, 0, 0, 0);
#else
  discard;
#endif
}

