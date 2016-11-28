import sys

import common_dirs


HEADER = """uniform vec2 screen_dim_RCP;
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

"""

LOOP1 = """#if NUM_VOLUMES >= {*1*}
uniform sampler3D volume_{*1*};
uniform vec3 volume_dimensions_{*1*};
uniform sampler1D transfer_function_{*1*};
#endif

"""

PART1 = """#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

vec4 applyTF(in sampler1D tex, in float intensity)
{
#if GLSL_VERSION >= 130
#if defined(MIP)
  return texture(tex, intensity);
#else
  vec4 res = texture(tex, intensity);
  res.a = res.a / sampling_rate;
  return res;
#endif
#else
#if defined(MIP)
  return texture1D(tex, intensity);
#else
  vec4 res = texture1D(tex, intensity);
  res.a = res.a / sampling_rate;
  return res;
#endif
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

void main()
{
#if NUM_VOLUMES > 0
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

  if (startRayPosition == exitRayPosition)
    discard;   // background
  else {
    vec4 result = vec4(0.0);

"""

LOOP2 = """#if NUM_VOLUMES >= {*1*}
    float ch{*1*}V = 0.0;
#endif
"""

LOOP3 = """#if NUM_VOLUMES >= {*1*}
    bool ch{*1*}Done = false;
#endif
"""

PART2 = """    vec3 rayVector = exitRayPosition - startRayPosition;
    vec3 numVoxels = abs(rayVector * volume_dimensions_1);
    float numVoxel = max(max(numVoxels.x, numVoxels.y), numVoxels.z);
    float stepSize = 1.0 / (sampling_rate * numVoxel);

    float currentRayLength = 0.0;
    float rayDepth = -1.0;
    bool finished = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float voxel;
        vec4 color = vec4(0.0);
        vec4 chColor;
        vec3 samplePos = startRayPosition + currentRayLength * rayVector;
        bool saturated = true;

"""

LOOP4 = """#if NUM_VOLUMES >= {*1*}

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch{*1*}Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_{*1*}, samplePos).r;
#else
          voxel = texture3D(volume_{*1*}, samplePos).r;
#endif
          if (voxel <= ch{*1*}V && ch{*1*}V >= local_MIP_threshold) {
            ch{*1*}Done = true;
          } else if (voxel > ch{*1*}V) {
            ch{*1*}V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch{*1*}Done;
#else
        if (ch{*1*}V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_{*1*}, samplePos).r;
#else
          voxel = texture3D(volume_{*1*}, samplePos).r;
#endif
          if (voxel > ch{*1*}V) {
            rayDepth = currentRayLength;
            ch{*1*}V = voxel;
          }
        }
        saturated = saturated && ch{*1*}V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_{*1*}, samplePos).r;
#else
        voxel = texture3D(volume_{*1*}, samplePos).r;
#endif
        chColor = applyTF(transfer_function_{*1*}, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


"""

PART3 = """#ifdef MIP
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
        currentRayLength += stepSize;
        finished = finished || (currentRayLength > 1.0);
      }
    }

"""

LOOP5 = """#if NUM_VOLUMES >= {*1*}
  result = max(result, applyTF(transfer_function_{*1*}, ch{*1*}V));
#endif
"""

FOOT = """#ifdef RESULT_OPAQUE
    result.a = 1.0;
#endif


    if (rayDepth >= 0.0) {
      //http://www.opengl.org/archives/resources/faq/technical/depthbuffer.htm
      // zw = a/ze + b;  ze = a/(zw - b);  a = f*n/(f-n);  b = 0.5*(f+n)/(f-n) + 0.5;
#if GLSL_VERSION >= 130
      float zeFront = texture(ray_entry_eye_coord, texCoords).z;
      float zeBack = texture(ray_exit_eye_coord, texCoords).z;
#else
      float zeFront = texture2D(ray_entry_eye_coord, texCoords).z;
      float zeBack = texture2D(ray_exit_eye_coord, texCoords).z;
#endif
      float ze = zeFront + rayDepth * (zeBack-zeFront);
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
  }
#endif
}

"""


def generate_volume_raycaster_frag(file: str, max_num_volumes: int):
    """

    """
    with open(file, mode='w', encoding='utf-8') as f:
        f.write(HEADER)

        for i in range(max_num_volumes):
            f.write(LOOP1.replace('{*1*}', "{0}".format(i+1)))

        f.write(PART1)

        f.write("#ifdef MIP\n")
        for i in range(max_num_volumes):
            f.write(LOOP2.replace('{*1*}', "{0}".format(i+1)))
        f.write("#endif\n\n")

        f.write("#ifdef LOCAL_MIP\n")
        for i in range(max_num_volumes):
            f.write(LOOP3.replace('{*1*}', "{0}".format(i+1)))
        f.write("#endif\n\n")

        f.write(PART2)

        for i in range(max_num_volumes):
            f.write(LOOP4.replace('{*1*}', "{0}".format(i+1)))

        f.write(PART3)

        f.write("#ifdef MIP\n")
        for i in range(max_num_volumes):
            f.write(LOOP5.replace('{*1*}', "{0}".format(i+1)))
        f.write("#endif // MIP\n\n")

        f.write(FOOT)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        generate_volume_raycaster_frag(sys.argv[1], sys.argv[2])
    else:
        generate_volume_raycaster_frag(common_dirs.atlas_dir() + '/Resources/shader/volume_raycaster.frag', 20)
