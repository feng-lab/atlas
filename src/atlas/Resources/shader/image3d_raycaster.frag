uniform usampler3D page_directory;
uniform uvec3 page_directory_bases[LEVEL_COUNT];
uniform usampler3D page_table_cache;
uniform uvec3 page_table_block_size;
uniform sampler3D image_cache;
uniform uvec3 image_dimensions[LEVEL_COUNT];
uniform float voxel_world_sizes[LEVEL_COUNT];
uniform uvec3 image_block_size;
uniform vec3 image_address_to_normalized_texture_coord;

uniform float sampling_rate;
#ifdef ISO
uniform float iso_value;
#endif
#ifdef LOCAL_MIP
uniform float local_MIP_threshold;
#endif
uniform float ze_to_zw_a;
uniform float ze_to_zw_b;
uniform float ze_to_screen_pixel_voxel_size;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_exit_tex_coord;

uniform sampler2D last_ray_depth;
uniform sampler2D last_color;

uniform sampler3D volume;
uniform sampler1D transfer_function;

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

vec4 compositeDVR(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0)
    rayDepth = currentRayLength;

  vec4 result = vec4(0.0);

  result.a = curResult.a + (1.0 -curResult.a) * color.a;
  result.rgb = (curResult.rgb * curResult.a + (1.0 - curResult.a) * color.a * color.rgb) / result.a;

  return result;
}

vec4 compositeISO(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth, in float isoValue)
{
  vec4 result = curResult;
  float epsilon = 0.02;
  if (color.a >= isoValue-epsilon && color.a <= isoValue+epsilon) {
    result = color;
    result.a = 1.0;
    rayDepth = currentRayLength;
  }
  return result;
}

vec4 compositeXRay(in vec4 curResult, in vec4 color, in float currentRayLength, inout float rayDepth)
{
  if (rayDepth < 0.0)
    rayDepth = currentRayLength;
  return curResult + color;
}

#ifdef MIP
void sampleVolume(in vec3 startRayPosition, in vec3 exitRayPosition, in float stepSize,
  inout float currentRayLength, inout bool finished, inout float ch1V, inout float rayDepth)
#else
void sampleVolume(in vec3 startRayPosition, in vec3 exitRayPosition, in float stepSize,
  inout float currentRayLength, inout bool finished, inout vec4 result, inout float rayDepth)
#endif
{
  for (int loop0=0; !finished && loop0<255; loop0++) {
    for (int loop1=0; !finished && loop1<255; loop1++) {
      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);

      float voxel = texture(volume, samplePos).r;

#ifdef MIP
#ifdef LOCAL_MIP
      if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
        finished = true;
      } else if (voxel > ch1V) {
        ch1V = voxel;
        rayDepth = currentRayLength;
      }
#else
      if (voxel > ch1V) {
        ch1V = voxel;
        rayDepth = currentRayLength;
      }
      finished = ch1V >= 1.0;
#endif
#else
      vec4 color = texture(transfer_function, voxel);

      if (color.a > 0.0) {
        color.a / sampling_rate;
        result = COMPOSITING(result, color, currentRayLength, rayDepth);
        if (result.a >= 1.0) {
          result.a = 1.0;
          finished = true;
        }
      }
#endif // MIP

      currentRayLength += stepSize;
      finished = finished || (currentRayLength > 1.0);
    }
  }
}

#ifdef MIP
void sampleBlock(in uvec4 pageTableEntry, in int curLevel, in uvec3 pageTableCoord,
  in vec3 startRayPosition, in vec3 exitRayPosition, in float stepSize,
  inout float currentRayLength, inout bool finished, inout float ch1V, inout float rayDepth)
#else
void sampleBlock(in uvec4 pageTableEntry, in int curLevel, in uvec3 pageTableCoord,
  in vec3 startRayPosition, in vec3 exitRayPosition, in float stepSize,
  inout float currentRayLength, inout bool finished, inout vec4 result, inout float rayDepth)
#endif
{
  vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
  uvec3 voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
  bool blockFinished = finished || voxelCoord / image_block_size != pageTableCoord || currentRayLength > 1.0;

  for (int loop0=0; !blockFinished && loop0<255; loop0++) {
    vec3 fFracVoxelCoord = samplePos * image_dimensions[curLevel] - vec3(voxelCoord);
    vec3 voxelAddress = pageTableEntry.xyz + voxelCoord % image_block_size + fFracVoxelCoord + 2.0;
    float voxel = texture(image_cache, voxelAddress * image_address_to_normalized_texture_coord).r;

#ifdef MIP
#ifdef LOCAL_MIP
    if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
      finished = true;
    } else if (voxel > ch1V) {
      ch1V = voxel;
      rayDepth = currentRayLength;
    }
#else
    if (voxel > ch1V) {
      ch1V = voxel;
      rayDepth = currentRayLength;
    }
    finished = ch1V >= 1.;
#endif
#else
    vec4 color = texture(transfer_function, voxel);

    if (color.a > 0.0) {
      color.a /= sampling_rate;
      result = COMPOSITING(result, color, currentRayLength, rayDepth);
      if (result.a >= 1.0) {
        result.a = 1.0;
        finished = true;
      }
    }
#endif // MIP
    currentRayLength += stepSize;

    samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
    voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
    blockFinished = finished || voxelCoord / image_block_size != pageTableCoord || currentRayLength > 1.0;
  }
}

#define UNMAPPED 0
#define EMPTY 40000
#define UINTMAX 4294967295U

void main()
{
  vec2 lastRayDepth = texelFetch(last_ray_depth, ivec2(gl_FragCoord.xy), 0).xy;
  vec4 result = texelFetch(last_color, ivec2(gl_FragCoord.xy), 0);
  float currentRayLength = lastRayDepth.x;
  float rayDepth = lastRayDepth.y;
  if (currentRayLength >= 1.0) {
    FragData0 = result;
    FragData1.xy = lastRayDepth;
    return;
  }
  if (currentRayLength == 0.0) {
    rayDepth = -1.0;
  }

  vec4 entryTexCoordAndZ = texelFetch(ray_entry_tex_coord, ivec2(gl_FragCoord.xy), 0);
  vec4 exitTexCoordAndZ = texelFetch(ray_exit_tex_coord, ivec2(gl_FragCoord.xy), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    // vec4 result = vec4(0.0);
#ifdef MIP
    float ch1V = result.r;
#endif

    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
    float zeFront = entryTexCoordAndZ.w;
    float zeBack = exitTexCoordAndZ.w;
    int curLevel = 0;

    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * image_dimensions[curLevel]);
    float stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

    // float currentRayLength = 0.0;
    // float rayDepth = -1.0;

    uvec3 pageDirAddress = uvec3(UINTMAX,UINTMAX,UINTMAX);
    uvec4 pageDirEntry = uvec4(UINTMAX,UINTMAX,UINTMAX,UINTMAX);

    bool finished = false;
    bool hitMissedBlock = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * ze_to_screen_pixel_voxel_size;
        while (curLevel+1 < LEVEL_COUNT && voxel_world_sizes[curLevel+1] <= desiredVoxelSize) {
          ++curLevel;
          numVoxels = abs(rayVector * image_dimensions[curLevel]);
          stepSize = 1.0 / (sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
        }

        if (curLevel + 1 == LEVEL_COUNT) {
#ifdef MIP
          sampleVolume(startRayPosition, exitRayPosition, stepSize,
                       currentRayLength, finished, ch1V, rayDepth);
#else
          sampleVolume(startRayPosition, exitRayPosition, stepSize,
                       currentRayLength, finished, result, rayDepth);
#endif
          // finished = true;  // should be true
          break;
        }

        vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
        uvec3 voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);

        uvec3 pageTableCoord = voxelCoord / image_block_size;
        uvec3 curPageDirAddress = page_directory_bases[curLevel] + pageTableCoord / page_table_block_size;
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
          pageDirEntry = texelFetch(page_directory, ivec3(pageDirAddress), 0);
        }
        uint pagingFlag = pageDirEntry.w;
        if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
          uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + pageTableCoord % page_table_block_size), 0);
          pagingFlag = pageTableEntry.w;
          if (pagingFlag != UNMAPPED && pagingFlag != EMPTY) {
#ifdef MIP
            sampleBlock(pageTableEntry, curLevel, pageTableCoord,
              startRayPosition, exitRayPosition, stepSize,
              currentRayLength, finished, ch1V, rayDepth);
#else
            sampleBlock(pageTableEntry, curLevel, pageTableCoord,
              startRayPosition, exitRayPosition, stepSize,
              currentRayLength, finished, result, rayDepth);
#endif
          } else if (pagingFlag == UNMAPPED) { // unmapped block
            hitMissedBlock = true;
          } else { // empty block
            // goto next block
            do {
              currentRayLength += stepSize;
              samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
              voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
            } while (voxelCoord / image_block_size == pageTableCoord && currentRayLength <= 1.0);
          }
        } else if (pagingFlag == UNMAPPED) { // unmapped page directory
          hitMissedBlock = true;
        } else { // empty page directory
          do { // skip empty space page directory entry
            currentRayLength += stepSize;
            samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
            voxelCoord = clamp(uvec3(samplePos * image_dimensions[curLevel]), uvec3(0, 0, 0), image_dimensions[curLevel] - 1);
          } while (page_directory_bases[curLevel] + voxelCoord / image_block_size / page_table_block_size == pageDirAddress && currentRayLength <= 1.0);
        }

        finished = finished || hitMissedBlock || (currentRayLength > 1.0);
      } // for
    }

    if (hitMissedBlock && currentRayLength < 1.0) {
#ifdef MIP
      FragData0.x = ch1V;
#else
      FragData0 = result;
#endif
      FragData1.xy = vec2(currentRayLength, rayDepth);
      return;
    }

#ifdef MIP
    result = texture(transfer_function, ch1V);
#endif // MIP

#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif

    float fragDepth;
    if (rayDepth >= 0.0) {
      fragDepth = ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + ze_to_zw_b;
    } else {
#ifdef RESULT_OPAQUE
      fragDepth = ze_to_zw_a / zeFront + ze_to_zw_b;
#else
      fragDepth = 1.0;
#endif
    }

    result.rgb *= result.a;
    FragData0 = result;
    FragData1.xy = vec2(1.0, fragDepth);
  }
}

