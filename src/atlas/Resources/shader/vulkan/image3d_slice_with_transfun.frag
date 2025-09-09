#version 450

layout(constant_id = 70) const int LEVEL_COUNT = 1;
layout(constant_id = 71) const bool RESULT_OPAQUE = false;

layout(set = 0, binding = 0) uniform usampler3D page_directory;
layout(set = 0, binding = 1) uniform usampler3D page_table_cache;
layout(set = 0, binding = 2) uniform sampler3D  image_cache;
layout(set = 0, binding = 3) uniform sampler3D  volume;
layout(set = 0, binding = 4) uniform sampler1D  transfer_function;

layout(std140, set = 2, binding = 2) uniform PageData {
  uvec3 page_directory_bases[LEVEL_COUNT];
  uvec3 page_table_block_size;
  uvec3 image_dimensions[LEVEL_COUNT];
  float voxel_world_sizes[LEVEL_COUNT];
  uvec3 image_block_size;
  vec3  image_address_to_normalized_texture_coord;
  float ze_to_screen_pixel_voxel_size;
} pg;

layout(location = 0) in vec3 texCoord0;
layout(location = 1) in vec4 eyeCoord;

layout(location = 0) out vec4 FragData0;

const uint UNMAPPED = 0u;
const uint EMPTY    = 40000u;

void main()
{
  float desiredVoxelSize = eyeCoord.z * pg.ze_to_screen_pixel_voxel_size;
  int curLevel = 0;
  while (curLevel + 1 < LEVEL_COUNT && pg.voxel_world_sizes[curLevel + 1] <= desiredVoxelSize) {
    ++curLevel;
  }

  vec4 color = vec4(0.0);

  if (curLevel + 1 == LEVEL_COUNT) {
    color = texture(transfer_function, texture(volume, texCoord0).r);
    if (RESULT_OPAQUE) {
      if (color.a == 0.0) color = vec4(0.0);
      color.a = 1.0;
    } else {
      if (color.a == 0.0) color = vec4(0.0);
      color.rgb *= color.a;
    }
    FragData0 = color;
    return;
  }

  vec3 voxelAddress;
  uvec3 voxelCoord = clamp(uvec3(texCoord0 * pg.image_dimensions[curLevel]), uvec3(0u), pg.image_dimensions[curLevel] - 1u);
  vec3 fFracVoxelCoord = texCoord0 * pg.image_dimensions[curLevel] - vec3(voxelCoord);

  uvec3 pageTableCoord = voxelCoord / pg.image_block_size;
  uvec4 pageDirEntry = texelFetch(page_directory, ivec3(pg.page_directory_bases[curLevel] + pageTableCoord / pg.page_table_block_size), 0);
  uint pagingFlag = pageDirEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % pg.page_table_block_size), 0);
    pagingFlag = pageTableEntry.w;
    if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
      voxelAddress = pageTableEntry.xyz + voxelCoord % pg.image_block_size + fFracVoxelCoord + 2.0;
      color = texture(transfer_function, texture(image_cache, voxelAddress * pg.image_address_to_normalized_texture_coord).r);
      if (RESULT_OPAQUE) {
        if (color.a == 0.0) color = vec4(0.0);
        color.a = 1.0;
      } else {
        if (color.a == 0.0) color = vec4(0.0);
        color.rgb *= color.a;
      }
    }
  }
  if (pagingFlag == EMPTY) {
    color = texture(transfer_function, 0.0);
    if (RESULT_OPAQUE) {
      if (color.a == 0.0) color = vec4(0.0);
      color.a = 1.0;
    } else {
      if (color.a == 0.0) color = vec4(0.0);
      color.rgb *= color.a;
    }
  }

  FragData0 = color;
}

