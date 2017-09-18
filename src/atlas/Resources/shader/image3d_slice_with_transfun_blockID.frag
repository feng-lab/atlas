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

uniform isampler3D page_directory;
uniform ivec3 page_directory_bases[LEVEL_COUNT];
uniform isampler3D page_table_cache;
uniform ivec3 page_table_block_size;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform ivec3 image_block_size;
uniform uvec4 pos_to_block_ids[LEVEL_COUNT];

uniform float ze_to_screen_pixel_voxel_size;

#if GLSL_VERSION >= 330
layout(location = 0) out uvec4 FragData0;
layout(location = 1) out uvec4 FragData1;
#elif GLSL_VERSION >= 130
out uvec4 FragData0;  // call glBindFragDataLocation before linking
out uvec4 FragData1;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#define FragData1 gl_FragData[1]
#endif

#define UNMAPPED 0
#define EMPTY 40000

void main()
{
#if NUM_VOLUMES > 0
  float desiredVoxelSize = eyeCoord.z * ze_to_screen_pixel_voxel_size;
  int curLevel = 0;
  while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
    ++curLevel;
  }

  ivec3 pageTableCoord = ivec3(texCoord0 * image_dimensions[curLevel]) / image_block_size;
  uint blockID = pos_to_block_ids[curLevel].w + uint(pageTableCoord.x) + pos_to_block_ids[curLevel].y * uint(pageTableCoord.y) + pos_to_block_ids[curLevel].z * uint(pageTableCoord.z);
#if GLSL_VERSION >= 130
  ivec4 pageDirEntry = texelFetch(page_directory, page_directory_bases[curLevel] + pageTableCoord / page_table_block_size, 0);
#else
  ivec4 pageDirEntry = texelFetch3D(page_directory, page_directory_bases[curLevel] + pageTableCoord / page_table_block_size, 0);
#endif
  int pagingFlag = pageDirEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
#if GLSL_VERSION >= 130
    ivec4 pageTableEntry = texelFetch(page_table_cache, pageDirEntry.xyz + pageTableCoord % page_table_block_size, 0);
#else
    ivec4 pageTableEntry = texelFetch3D(page_table_cache, pageDirEntry.xyz + pageTableCoord % page_table_block_size, 0);
#endif
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
      FragData1 = uvec4(blockID, 0, 0, 0);
    }
  }
  if (pagingFlag == UNMAPPED) {
    FragData0 = uvec4(blockID, 0, 0, 0);
  }
#else
  discard;
#endif
}

