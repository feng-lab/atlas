uniform sampler3D page_directory;
uniform ivec3 page_directory_base[LEVEL_COUNT];
uniform ivec3 page_directory_dimensions[LEVEL_COUNT];
uniform sampler3D page_table_cache;
uniform ivec3 page_table_dimensions[LEVEL_COUNT];
uniform ivec3 page_table_block_size = ivec3(32, 32, 32);
uniform sampler3D voxel_cache;
uniform ivec3 voxel_dimensions[LEVEL_COUNT];
uniform float voxel_size[LEVEL_COUNT];
uniform ivec3 voxel_block_size = ivec3(32, 32, 32);

uniform vec2 screen_dim_RCP;
uniform float minus_near_dist;
uniform float sampling_rate;
#ifdef ISO
uniform float iso_value;
#endif
#ifdef LOCAL_MIP
uniform float local_MIP_threshold;
#endif
uniform float ze_to_zw_a;
uniform float ze_to_zw_b;

uniform sampler2D ray_entry_tex_coord;
uniform sampler2D ray_entry_eye_coord;
uniform sampler2D ray_exit_tex_coord;
uniform sampler2D ray_exit_eye_coord;

uniform TF_SAMPLER_TYPE transfer_function;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
layout(location = 1) out ivec4 FragData1;
layout(location = 2) out ivec4 FragData2;
layout(location = 3) out ivec4 FragData3;
layout(location = 4) out ivec4 FragData4;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
out ivec4 FragData1;  // call glBindFragDataLocation before linking
out ivec4 FragData2;  // call glBindFragDataLocation before linking
out ivec4 FragData3;  // call glBindFragDataLocation before linking
out ivec4 FragData4;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#define FragData1 gl_FragData[1]
#define FragData0 gl_FragData[2]
#define FragData1 gl_FragData[3]
#define FragData1 gl_FragData[4]
#endif

vec4 applyTF(in sampler1D tex, in float intensity)
{
#if GLSL_VERSION >= 130
  return texture(tex, intensity);
#else
  return texture1D(tex, intensity);
#endif
}

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

#define MAPPED 0
#define UNMAPPED 1
#define EMPTY 2

void main()
{
  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;
#if GLSL_VERSION >= 130
  vec4 entryTexCoordAndZ = texture(ray_entry_tex_coord, texCoords);
  vec4 exitTexCoordAndZ = texture(ray_exit_tex_coord, texCoords);
#else
  vec4 entryTexCoordAndZ = texture2D(ray_entry_tex_coord, texCoords);
  vec4 exitTexCoordAndZ = texture2D(ray_exit_tex_coord, texCoords);
#endif
  vec3 startRayPosition = entryTexCoordAndZ.xyz;
  vec3 exitRayPosition = exitTexCoordAndZ.xyz;

  if (startRayPosition == exitRayPosition) {
    discard;   // background
  } else {
    vec4 result = vec4(0.0);

#ifdef MIP
    float ch1V = 0.0;
#endif

#ifdef LOCAL_MIP
    bool ch1Done = false;
#endif

    //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
    // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
    float zeFront = texture(ray_entry_eye_coord, texCoords).z;
    float zeBack = texture(ray_exit_eye_coord, texCoords).z;
#else
    float zeFront = texture2D(ray_entry_eye_coord, texCoords).z;
    float zeBack = texture2D(ray_exit_eye_coord, texCoords).z;
#endif
    float ze = zeFront;
    int curLevel = 0;
    float zeLengthRCP = 1.0 / (zeBack - zeFront);
    
    uint missBlockIDs[4] = uint[4](0,0,0,0);
    int missBlockIDsIndex = 0;
    uint usedBlockIDs[12] = uint[12](0,0,0,0, 0,0,0,0, 0,0,0,0);
    int usedBlockIDsIndex = 0;

    vec3 rayVector = exitRayPosition - startRayPosition;
    float maxRayLength = length(rayVector);

    float currentRayLength = 0.0;
    float rayDepth = -1.0;
    bool finished = false;

    ivec3 pageDirAddress = ivec3(-1,-1,-1);
    ivec4 pageDirEntry = ivec4(-1,-1,-1,-1);
    ivec3 pageTableAddress = ivec3(-1,-1,-1);
    ivec4 pageTableEntry = ivec4(-1,-1,-1,-1);

    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float desiredVoxelSize = ze / minus_near_dist;
        while (voxel_size[curLevel] <= desiredVoxelSize && curLevel + 1 < LEVEL_COUNT) {
          ++curLevel;
        }
        float stepSize = -sampling_rate * voxel_size[curLevel];
        vec3 samplePos = startRayPosition + (ze - zeFront) * zeLengthRCP * rayVector;

        float voxel = 0;
        vec4 color = vec4(0.0);
        vec4 chColor;
        bool saturated = true;

        ivec3 curPageDirAddress = page_directory_base[curLevel] + samplePos * page_directory_dimensions[curResult];
        if (curPageDirAddress != pageDirAddress) {
          pageDirAddress = curPageDirAddress;
          pageDirEntry = texelFetch(page_directory, pageDirAddress);
        }
        int pagingFlag = pageDirEntry.w;
        if (pagingFlag == MAPPED) {
          ivec3 curPageTableAddress = pageDirEntry.xyz + (samplePos * page_table_dimensions[curLevel]) % page_table_block_size;
          if (curPageTableAddress != pageTableAddress) {
            pageTableAddress = curPageTableAddress;
            pageTableEntry = texelFetch(page_table_cache, pageTableAddress);
          }
          pagingFlag = pageTableEntry.w;
          if (pagingFlag == MAPPED) {
            ivec3 voxelAddress = pageTableEntry.xyz + (samplePos * voxel_dimensions[curLevel]) % voxel_block_size;
            voxel = texelFetch(voxel_cache, voxelAddress);
            if (usedBlockIDsIndex < 12) {
              int blockID = 
              if (usedBlockIDsIndex == 0 || blockID != usedBlockIDs[usedBlockIDsIndex-1]) {
                usedBlockIDs[usedBlockIDsIndex++] = blockID;
              }
            }
          } else {
            // skip empty space page table entry recursive
            
          }
        } else {
          // skip empty space page directory entry

        }
        if (pagingFlag == UNMAPPED && missBlockIDsIndex < 4) {
          int blockID = 
          if (missBlockIDsIndex == 0 || blockID != missBlockIDs[missBlockIDsIndex-1]) {
            missBlockIDs[missBlockIDsIndex++] = blockID;
          }
        }

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch1Done) {
          if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
            ch1Done = true;
          } else if (voxel > ch1V) {
            ch1V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch1Done;
#else
        if (voxel > ch1V) {
          rayDepth = currentRayLength;
          ch1V = voxel;
        }
        saturated = saturated && ch1V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_1, voxel);
        chColor.a /= sampling_rate;

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP


#ifdef MIP
        finished = saturated;
#else
        if (color.a > 0.0) {
          result = COMPOSITING(result, color, currentRayLength, rayDepth);
        }

        if (result.a >= 1.0) {
          result.a = 1.0;
          finished = true;
        }
#endif // MIP
        ze += stepSize;
        finished = finished || (ze < zeBack);
      }
    }

#ifdef MIP
  result = max(result, applyTF(transfer_function_1, ch1V));
#endif // MIP

#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif


    if (rayDepth >= 0.0) {
      gl_FragDepth = ze_to_zw_a / ze + ze_to_zw_b;
    } else {
#ifdef RESULT_OPAQUE
      gl_FragDepth = entryTexCoordAndZ.w;
#else
      gl_FragDepth = 1.0;
#endif
    }

    result.rgb *= result.a;
    FragData0 = result;
    FragData1 = uvec4(missBlockIDs[0], missBlockIDs[1], missBlockIDs[2], missBlockIDs[3]);
    FragData2 = uvec4(usedBlockIDs[0], usedBlockIDs[1], usedBlockIDs[2], usedBlockIDs[3]);
    FragData3 = uvec4(usedBlockIDs[4], usedBlockIDs[5], usedBlockIDs[6], usedBlockIDs[7]);
    FragData4 = uvec4(usedBlockIDs[8], usedBlockIDs[9], usedBlockIDs[10], usedBlockIDs[11]);
  }
}

