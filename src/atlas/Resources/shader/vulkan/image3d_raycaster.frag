#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

#include "include/raycaster_common.glslinc"

// Whether to force opaque output (MIP opaque/local MIP opaque parity)
layout(constant_id = 51) const bool RESULT_OPAQUE = false;

void main()
{
  vec2 last = texelFetch(last_ray_depth_tex, ivec2(gl_FragCoord.xy), 0).xy;
  vec4 result = texelFetch(last_color_tex, ivec2(gl_FragCoord.xy), 0);
  float currentRayLength = last.x;
  float rayDepth = last.y;
  if (currentRayLength >= 1.0) { FragData0 = result; FragData1.xy = last; return; }
  if (currentRayLength == 0.0) { rayDepth = -1.0; }

  vec4 entryTexCoordAndZ = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 0), 0);
  vec4 exitTexCoordAndZ  = texelFetch(ray_entry_exit_tex_coord, ivec3(gl_FragCoord.xy, 1), 0);
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition  = exitTexCoordAndZ.xyz;
  if (all(equal(startRayPosition, exitRayPosition))) { discard; }

  float zeFront = entryTexCoordAndZ.w;
  float zeBack  = exitTexCoordAndZ.w;
  int curLevel = 0;

  vec3 rayVector = exitRayPosition - startRayPosition;
  vec3 numVoxels = abs(rayVector * pg.levels[curLevel].image_dimensions.xyz);
  float stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));

  uvec3 pageDirAddress = uvec3(0xFFFFFFFFu);
  uvec4 pageDirEntry   = uvec4(0xFFFFFFFFu);

  bool finished = false;
  bool hitMissedBlock = false;
  float mipValue = result.r; // for MIP

  for (int loop0=0; !finished && loop0<255; ++loop0) {
    for (int loop1=0; !finished && loop1<255; ++loop1) {
      float desiredVoxelSize = mix(zeFront, zeBack, currentRayLength) * pg.ze_to_screen_pixel_voxel_size;
      while (curLevel + 1 < LEVEL_COUNT && pg.levels[curLevel+1].voxel_world_size_pad.x <= desiredVoxelSize) {
        ++curLevel;
        numVoxels = abs(rayVector * pg.levels[curLevel].image_dimensions.xyz);
        stepSize = 1.0 / (rp.sampling_rate * max(max(numVoxels.x, numVoxels.y), numVoxels.z));
      }

      if (curLevel + 1 == LEVEL_COUNT) {
        sampleVolumeNoPaging(startRayPosition, exitRayPosition, stepSize,
                             currentRayLength, finished, result, rayDepth, mipValue);
        break;
      }

      vec3 samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
      uvec3 voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);

      uvec3 pageTableCoord = voxelCoord / pg.image_block_size.xyz;
      uvec3 curPageDirAddress = pg.levels[curLevel].page_directory_base.xyz + pageTableCoord / pg.page_table_block_size.xyz;
      if (curPageDirAddress != pageDirAddress) {
        pageDirAddress = curPageDirAddress;
        pageDirEntry = texelFetch(page_directory, ivec3(pageDirAddress), 0);
      }
      uint pagingFlag = pageDirEntry.w;
      if (pagingFlag != 0u && pagingFlag != 40000u) {
        uvec4 pageTableEntry = texelFetch(page_table_cache, ivec3(pageDirEntry.xyz + (pageTableCoord % pg.page_table_block_size.xyz)), 0);
        pagingFlag = pageTableEntry.w;
        if (pagingFlag != 0u && pagingFlag != 40000u) {
          sampleBlock(pageTableEntry, curLevel, pageTableCoord,
                      startRayPosition, exitRayPosition, stepSize,
                      currentRayLength, finished, result, rayDepth, mipValue);
        } else if (pagingFlag == 0u) {
          hitMissedBlock = true;
        } else { // EMPTY block
          do {
            currentRayLength += stepSize;
            samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
            voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
          } while (voxelCoord / pg.image_block_size.xyz == pageTableCoord && currentRayLength <= 1.0);
        }
      } else if (pagingFlag == 0u) {
        hitMissedBlock = true;
      } else { // EMPTY page directory
        do {
          currentRayLength += stepSize;
          samplePos = mix(startRayPosition, exitRayPosition, currentRayLength);
          voxelCoord = clamp(uvec3(samplePos * pg.levels[curLevel].image_dimensions.xyz), uvec3(0u), pg.levels[curLevel].image_dimensions.xyz - 1u);
        } while (pg.levels[curLevel].page_directory_base.xyz + (voxelCoord / pg.image_block_size.xyz) / pg.page_table_block_size.xyz == pageDirAddress && currentRayLength <= 1.0);
      }
      finished = finished || hitMissedBlock || (currentRayLength > 1.0);
    }
  }

  if (hitMissedBlock && currentRayLength < 1.0) {
    if (RAY_MODE == 1) FragData0 = vec4(mipValue, 0, 0, 0);
    else FragData0 = result;
    FragData1.xy = vec2(currentRayLength, rayDepth);
    return;
  }

  if (RAY_MODE == 1) result = texture(transfer_function, mipValue);
  // Force opaque alpha when requested
  if (RESULT_OPAQUE) {
    result.a = 1.0;
  }
  // Pre-multiplied alpha for blending path
  result.rgb *= result.a;

  float fragDepth;
  if (rayDepth >= 0.0) fragDepth = rp.ze_to_zw_a / mix(zeFront, zeBack, rayDepth) + rp.ze_to_zw_b;
  else fragDepth = RESULT_OPAQUE ? (rp.ze_to_zw_a / zeFront + rp.ze_to_zw_b) : 1.0;

  FragData0 = result;
  FragData1.xy = vec2(1.0, fragDepth);
}
