#if GLSL_VERSION < 130
#extension GL_EXT_gpu_shader4 : enable
#define uint unsigned int
#endif

#if GLSL_VERSION >= 130
in vec3 texCoord0;
in vec4 eyeCoord;
#else
varying vec3 texCoord0;
varying vec4 eyeCoord;
#endif

uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform uvec3 pos_to_block_ids[LEVEL_COUNT];

uniform float ze_to_screen_pixel_voxel_size;

#if GLSL_VERSION >= 330
layout(location = 0) out uvec4 FragData0;
#elif GLSL_VERSION >= 130
out uvec4 FragData0;  // call glBindFragDataLocation before linking
#else
varying out uvec4 FragData0;  // call glBindFragDataLocationForce before linking
#endif

#define UNMAPPED 0
#define EMPTY 40000
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

  uvec3 pageTableCoord = uvec3(texCoord0 * image_dimensions[curLevel]) / image_block_size;
  uint blockID = pos_to_block_ids[curLevel].x + pageTableCoord.x + pos_to_block_ids[curLevel].y * pageTableCoord.y + pos_to_block_ids[curLevel].z * pageTableCoord.z;
#if GLSL_VERSION >= 130
  uvec4 pageDirEntry = texelFetch(page_directory, ivec3(page_directory_bases[curLevel] + pageTableCoord / page_table_block_size), 0);
#else
  uvec4 pageDirEntry = texelFetch3D(page_directory, ivec3(page_directory_bases[curLevel] + pageTableCoord / page_table_block_size), 0);
#endif
  uint pagingFlag = pageDirEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
#if GLSL_VERSION >= 130
    uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
#else
    uvec4 pageTableEntry = texelFetch3D(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
#endif
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
      FragData0 = uvec4(blockID, 0, 0, 0);
    }
  } else if (pagingFlag == UNMAPPED) {
    FragData0 = uvec4(blockID, 0, 0, 0);
  }
#else
  discard;
#endif
}

