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

uniform sampler1D transfer_function;

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
vec3 fVoxelCoord = texCoord0 * image_dimensions[curLevel];
ivec3 pageTableCoord = ivec3(fVoxelCoord) / image_block_size;
ivec4 pageDirEntry = texelFetch(page_directory, page_directory_bases[curLevel] + pageTableCoord / page_table_block_size, 0);
int pagingFlag = pageDirEntry.w;
if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
  ivec4 pageTableEntry = texelFetch(page_table_cache, pageDirEntry.xyz + pageTableCoord % page_table_block_size, 0);
  pagingFlag = pageTableEntry.w;
  if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
    vec3 voxelAddress = pageTableEntry.xyz + mod(fVoxelCoord, image_block_size);
#if GLSL_VERSION >= 130
    color = texture(transfer_function, texture(image_cache, (voxelAddress*2.0+1.0)*image_address_to_normalized_texture_coord).r);
#else
    color = texture1D(transfer_function, texture3D(image_cache, (voxelAddress*2.0+1.0)*image_address_to_normalized_texture_coord).r);
#endif
    if (color.a == 0.0) {
      color = vec4(0.0);
    }
  }
}

#ifdef RESULT_OPAQUE
  color.a = 1.0;
#else
  color.rgb *= color.a;
#endif

  FragData0 = color;
#else
  discard;
#endif
}

