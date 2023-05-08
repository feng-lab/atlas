in vec3 texCoord0;
in vec4 eyeCoord;

uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform sampler3D image_cache;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform vec3 image_address_to_normalized_texture_coord;

uniform float ze_to_screen_pixel_voxel_size;

uniform sampler3D volume;
uniform sampler1D transfer_function;

layout(location = 0) out vec4 FragData0;

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

  vec4 color = vec4(0.0);

  if (curLevel + 1 == LEVEL_COUNT) {
    color = texture(transfer_function, texture(volume, texCoord0).r);

#ifdef RESULT_OPAQUE
    if (color.a == 0.0) {
      color = vec4(0.0);
    }
    color.a = 1.0;
#else
    color.rgb *= color.a;
#endif
    FragData0 = color;
    return;
  }

  vec3 voxelAddress;

  uvec3 voxelCoord = clamp(uvec3(texCoord0 * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
  vec3 fFracVoxelCoord = texCoord0 * image_dimensions[curLevel] - vec3(voxelCoord);

  uvec3 pageTableCoord = voxelCoord / image_block_size;
  uvec4 pageDirEntry = texelFetch(page_directory, ivec3(page_directory_bases[curLevel] + pageTableCoord / page_table_block_size), 0);
  uint pagingFlag = pageDirEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
      voxelAddress = pageTableEntry.xyz + voxelCoord % image_block_size + fFracVoxelCoord + 2.0 + 0.5;
      color = texture(transfer_function, texture(image_cache, voxelAddress * image_address_to_normalized_texture_coord).r);

#ifdef RESULT_OPAQUE
      if (color.a == 0.0) {
        color = vec4(0.0);
      }
      color.a = 1.0;
#else
      color.rgb *= color.a;
#endif
    }
  }
  if (pagingFlag == EMPTY) {
    color = texture(transfer_function, 0);

#ifdef RESULT_OPAQUE
    if (color.a == 0.0) {
      color = vec4(0.0);
    }
    color.a = 1.0;
#else
    color.rgb *= color.a;
#endif
  }

  FragData0 = color;
#else
  discard;
#endif
}

