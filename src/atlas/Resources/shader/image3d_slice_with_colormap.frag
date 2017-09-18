#if GLSL_VERSION < 130
#extension GL_EXT_gpu_shader4 : enable
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
uniform sampler3D image_cache;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform ivec3 image_block_size;
uniform vec3 image_address_to_normalized_texture_coord;

uniform float ze_to_screen_pixel_voxel_size;

uniform sampler1D colormap;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
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

  vec4 color = vec4(0.0);
  vec3 voxelAddress;
#if GLSL_VERSION >= 130
  vec3 fFracVoxelCoord = modf(texCoord0 * image_dimensions[curLevel], voxelAddress);
  ivec3 voxelCoord = ivec3(voxelAddress);
#else
  ivec3 voxelCoord = ivec3(texCoord0 * image_dimensions[curLevel]);
  vec3 fFracVoxelCoord = texCoord0 * image_dimensions[curLevel] - vec3(voxelCoord);
#endif
  ivec3 pageTableCoord = voxelCoord / image_block_size;
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
      voxelAddress = pageTableEntry.xyz + voxelCoord % image_block_size + fFracVoxelCoord + 1.0;
#if GLSL_VERSION >= 130
      color = texture(colormap, texture(image_cache, (voxelAddress*2.0+1.0)*image_address_to_normalized_texture_coord).r);
#else
      color = texture1D(colormap, texture3D(image_cache, (voxelAddress*2.0+1.0)*image_address_to_normalized_texture_coord).r);
#endif
      color.rgb *= color.a;
    }
  }

  FragData0 = color;
#else
  discard;
#endif
}



