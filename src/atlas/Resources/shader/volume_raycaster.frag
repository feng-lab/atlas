uniform vec2 screen_dim_RCP;
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

#if NUM_VOLUMES >= 1
uniform sampler3D volume_1;
uniform vec3 volume_dimensions_1;
uniform sampler1D transfer_function_1;
#endif

#if NUM_VOLUMES >= 2
uniform sampler3D volume_2;
uniform vec3 volume_dimensions_2;
uniform sampler1D transfer_function_2;
#endif

#if NUM_VOLUMES >= 3
uniform sampler3D volume_3;
uniform vec3 volume_dimensions_3;
uniform sampler1D transfer_function_3;
#endif

#if NUM_VOLUMES >= 4
uniform sampler3D volume_4;
uniform vec3 volume_dimensions_4;
uniform sampler1D transfer_function_4;
#endif

#if NUM_VOLUMES >= 5
uniform sampler3D volume_5;
uniform vec3 volume_dimensions_5;
uniform sampler1D transfer_function_5;
#endif

#if NUM_VOLUMES >= 6
uniform sampler3D volume_6;
uniform vec3 volume_dimensions_6;
uniform sampler1D transfer_function_6;
#endif

#if NUM_VOLUMES >= 7
uniform sampler3D volume_7;
uniform vec3 volume_dimensions_7;
uniform sampler1D transfer_function_7;
#endif

#if NUM_VOLUMES >= 8
uniform sampler3D volume_8;
uniform vec3 volume_dimensions_8;
uniform sampler1D transfer_function_8;
#endif

#if NUM_VOLUMES >= 9
uniform sampler3D volume_9;
uniform vec3 volume_dimensions_9;
uniform sampler1D transfer_function_9;
#endif

#if NUM_VOLUMES >= 10
uniform sampler3D volume_10;
uniform vec3 volume_dimensions_10;
uniform sampler1D transfer_function_10;
#endif

#if NUM_VOLUMES >= 11
uniform sampler3D volume_11;
uniform vec3 volume_dimensions_11;
uniform sampler1D transfer_function_11;
#endif

#if NUM_VOLUMES >= 12
uniform sampler3D volume_12;
uniform vec3 volume_dimensions_12;
uniform sampler1D transfer_function_12;
#endif

#if NUM_VOLUMES >= 13
uniform sampler3D volume_13;
uniform vec3 volume_dimensions_13;
uniform sampler1D transfer_function_13;
#endif

#if NUM_VOLUMES >= 14
uniform sampler3D volume_14;
uniform vec3 volume_dimensions_14;
uniform sampler1D transfer_function_14;
#endif

#if NUM_VOLUMES >= 15
uniform sampler3D volume_15;
uniform vec3 volume_dimensions_15;
uniform sampler1D transfer_function_15;
#endif

#if NUM_VOLUMES >= 16
uniform sampler3D volume_16;
uniform vec3 volume_dimensions_16;
uniform sampler1D transfer_function_16;
#endif

#if NUM_VOLUMES >= 17
uniform sampler3D volume_17;
uniform vec3 volume_dimensions_17;
uniform sampler1D transfer_function_17;
#endif

#if NUM_VOLUMES >= 18
uniform sampler3D volume_18;
uniform vec3 volume_dimensions_18;
uniform sampler1D transfer_function_18;
#endif

#if NUM_VOLUMES >= 19
uniform sampler3D volume_19;
uniform vec3 volume_dimensions_19;
uniform sampler1D transfer_function_19;
#endif

#if NUM_VOLUMES >= 20
uniform sampler3D volume_20;
uniform vec3 volume_dimensions_20;
uniform sampler1D transfer_function_20;
#endif

#if GLSL_VERSION >= 330
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

#ifdef MIP
#if NUM_VOLUMES >= 1
    float ch1V = 0.0;
#endif
#if NUM_VOLUMES >= 2
    float ch2V = 0.0;
#endif
#if NUM_VOLUMES >= 3
    float ch3V = 0.0;
#endif
#if NUM_VOLUMES >= 4
    float ch4V = 0.0;
#endif
#if NUM_VOLUMES >= 5
    float ch5V = 0.0;
#endif
#if NUM_VOLUMES >= 6
    float ch6V = 0.0;
#endif
#if NUM_VOLUMES >= 7
    float ch7V = 0.0;
#endif
#if NUM_VOLUMES >= 8
    float ch8V = 0.0;
#endif
#if NUM_VOLUMES >= 9
    float ch9V = 0.0;
#endif
#if NUM_VOLUMES >= 10
    float ch10V = 0.0;
#endif
#if NUM_VOLUMES >= 11
    float ch11V = 0.0;
#endif
#if NUM_VOLUMES >= 12
    float ch12V = 0.0;
#endif
#if NUM_VOLUMES >= 13
    float ch13V = 0.0;
#endif
#if NUM_VOLUMES >= 14
    float ch14V = 0.0;
#endif
#if NUM_VOLUMES >= 15
    float ch15V = 0.0;
#endif
#if NUM_VOLUMES >= 16
    float ch16V = 0.0;
#endif
#if NUM_VOLUMES >= 17
    float ch17V = 0.0;
#endif
#if NUM_VOLUMES >= 18
    float ch18V = 0.0;
#endif
#if NUM_VOLUMES >= 19
    float ch19V = 0.0;
#endif
#if NUM_VOLUMES >= 20
    float ch20V = 0.0;
#endif
#endif

#ifdef LOCAL_MIP
#if NUM_VOLUMES >= 1
    bool ch1Done = false;
#endif
#if NUM_VOLUMES >= 2
    bool ch2Done = false;
#endif
#if NUM_VOLUMES >= 3
    bool ch3Done = false;
#endif
#if NUM_VOLUMES >= 4
    bool ch4Done = false;
#endif
#if NUM_VOLUMES >= 5
    bool ch5Done = false;
#endif
#if NUM_VOLUMES >= 6
    bool ch6Done = false;
#endif
#if NUM_VOLUMES >= 7
    bool ch7Done = false;
#endif
#if NUM_VOLUMES >= 8
    bool ch8Done = false;
#endif
#if NUM_VOLUMES >= 9
    bool ch9Done = false;
#endif
#if NUM_VOLUMES >= 10
    bool ch10Done = false;
#endif
#if NUM_VOLUMES >= 11
    bool ch11Done = false;
#endif
#if NUM_VOLUMES >= 12
    bool ch12Done = false;
#endif
#if NUM_VOLUMES >= 13
    bool ch13Done = false;
#endif
#if NUM_VOLUMES >= 14
    bool ch14Done = false;
#endif
#if NUM_VOLUMES >= 15
    bool ch15Done = false;
#endif
#if NUM_VOLUMES >= 16
    bool ch16Done = false;
#endif
#if NUM_VOLUMES >= 17
    bool ch17Done = false;
#endif
#if NUM_VOLUMES >= 18
    bool ch18Done = false;
#endif
#if NUM_VOLUMES >= 19
    bool ch19Done = false;
#endif
#if NUM_VOLUMES >= 20
    bool ch20Done = false;
#endif
#endif

    vec3 rayVector = exitRayPosition - startRayPosition;
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

#if NUM_VOLUMES >= 1

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch1Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_1, samplePos).r;
#else
          voxel = texture3D(volume_1, samplePos).r;
#endif
          if (voxel <= ch1V && ch1V >= local_MIP_threshold) {
            ch1Done = true;
          } else if (voxel > ch1V) {
            ch1V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch1Done;
#else
        if (ch1V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_1, samplePos).r;
#else
          voxel = texture3D(volume_1, samplePos).r;
#endif
          if (voxel > ch1V) {
            rayDepth = currentRayLength;
            ch1V = voxel;
          }
        }
        saturated = saturated && ch1V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_1, samplePos).r;
#else
        voxel = texture3D(volume_1, samplePos).r;
#endif
        chColor = applyTF(transfer_function_1, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 2

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch2Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_2, samplePos).r;
#else
          voxel = texture3D(volume_2, samplePos).r;
#endif
          if (voxel <= ch2V && ch2V >= local_MIP_threshold) {
            ch2Done = true;
          } else if (voxel > ch2V) {
            ch2V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch2Done;
#else
        if (ch2V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_2, samplePos).r;
#else
          voxel = texture3D(volume_2, samplePos).r;
#endif
          if (voxel > ch2V) {
            rayDepth = currentRayLength;
            ch2V = voxel;
          }
        }
        saturated = saturated && ch2V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_2, samplePos).r;
#else
        voxel = texture3D(volume_2, samplePos).r;
#endif
        chColor = applyTF(transfer_function_2, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 3

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch3Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_3, samplePos).r;
#else
          voxel = texture3D(volume_3, samplePos).r;
#endif
          if (voxel <= ch3V && ch3V >= local_MIP_threshold) {
            ch3Done = true;
          } else if (voxel > ch3V) {
            ch3V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch3Done;
#else
        if (ch3V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_3, samplePos).r;
#else
          voxel = texture3D(volume_3, samplePos).r;
#endif
          if (voxel > ch3V) {
            rayDepth = currentRayLength;
            ch3V = voxel;
          }
        }
        saturated = saturated && ch3V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_3, samplePos).r;
#else
        voxel = texture3D(volume_3, samplePos).r;
#endif
        chColor = applyTF(transfer_function_3, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 4

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch4Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_4, samplePos).r;
#else
          voxel = texture3D(volume_4, samplePos).r;
#endif
          if (voxel <= ch4V && ch4V >= local_MIP_threshold) {
            ch4Done = true;
          } else if (voxel > ch4V) {
            ch4V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch4Done;
#else
        if (ch4V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_4, samplePos).r;
#else
          voxel = texture3D(volume_4, samplePos).r;
#endif
          if (voxel > ch4V) {
            rayDepth = currentRayLength;
            ch4V = voxel;
          }
        }
        saturated = saturated && ch4V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_4, samplePos).r;
#else
        voxel = texture3D(volume_4, samplePos).r;
#endif
        chColor = applyTF(transfer_function_4, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 5

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch5Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_5, samplePos).r;
#else
          voxel = texture3D(volume_5, samplePos).r;
#endif
          if (voxel <= ch5V && ch5V >= local_MIP_threshold) {
            ch5Done = true;
          } else if (voxel > ch5V) {
            ch5V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch5Done;
#else
        if (ch5V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_5, samplePos).r;
#else
          voxel = texture3D(volume_5, samplePos).r;
#endif
          if (voxel > ch5V) {
            rayDepth = currentRayLength;
            ch5V = voxel;
          }
        }
        saturated = saturated && ch5V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_5, samplePos).r;
#else
        voxel = texture3D(volume_5, samplePos).r;
#endif
        chColor = applyTF(transfer_function_5, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 6

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch6Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_6, samplePos).r;
#else
          voxel = texture3D(volume_6, samplePos).r;
#endif
          if (voxel <= ch6V && ch6V >= local_MIP_threshold) {
            ch6Done = true;
          } else if (voxel > ch6V) {
            ch6V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch6Done;
#else
        if (ch6V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_6, samplePos).r;
#else
          voxel = texture3D(volume_6, samplePos).r;
#endif
          if (voxel > ch6V) {
            rayDepth = currentRayLength;
            ch6V = voxel;
          }
        }
        saturated = saturated && ch6V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_6, samplePos).r;
#else
        voxel = texture3D(volume_6, samplePos).r;
#endif
        chColor = applyTF(transfer_function_6, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 7

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch7Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_7, samplePos).r;
#else
          voxel = texture3D(volume_7, samplePos).r;
#endif
          if (voxel <= ch7V && ch7V >= local_MIP_threshold) {
            ch7Done = true;
          } else if (voxel > ch7V) {
            ch7V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch7Done;
#else
        if (ch7V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_7, samplePos).r;
#else
          voxel = texture3D(volume_7, samplePos).r;
#endif
          if (voxel > ch7V) {
            rayDepth = currentRayLength;
            ch7V = voxel;
          }
        }
        saturated = saturated && ch7V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_7, samplePos).r;
#else
        voxel = texture3D(volume_7, samplePos).r;
#endif
        chColor = applyTF(transfer_function_7, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 8

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch8Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_8, samplePos).r;
#else
          voxel = texture3D(volume_8, samplePos).r;
#endif
          if (voxel <= ch8V && ch8V >= local_MIP_threshold) {
            ch8Done = true;
          } else if (voxel > ch8V) {
            ch8V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch8Done;
#else
        if (ch8V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_8, samplePos).r;
#else
          voxel = texture3D(volume_8, samplePos).r;
#endif
          if (voxel > ch8V) {
            rayDepth = currentRayLength;
            ch8V = voxel;
          }
        }
        saturated = saturated && ch8V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_8, samplePos).r;
#else
        voxel = texture3D(volume_8, samplePos).r;
#endif
        chColor = applyTF(transfer_function_8, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 9

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch9Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_9, samplePos).r;
#else
          voxel = texture3D(volume_9, samplePos).r;
#endif
          if (voxel <= ch9V && ch9V >= local_MIP_threshold) {
            ch9Done = true;
          } else if (voxel > ch9V) {
            ch9V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch9Done;
#else
        if (ch9V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_9, samplePos).r;
#else
          voxel = texture3D(volume_9, samplePos).r;
#endif
          if (voxel > ch9V) {
            rayDepth = currentRayLength;
            ch9V = voxel;
          }
        }
        saturated = saturated && ch9V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_9, samplePos).r;
#else
        voxel = texture3D(volume_9, samplePos).r;
#endif
        chColor = applyTF(transfer_function_9, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 10

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch10Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_10, samplePos).r;
#else
          voxel = texture3D(volume_10, samplePos).r;
#endif
          if (voxel <= ch10V && ch10V >= local_MIP_threshold) {
            ch10Done = true;
          } else if (voxel > ch10V) {
            ch10V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch10Done;
#else
        if (ch10V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_10, samplePos).r;
#else
          voxel = texture3D(volume_10, samplePos).r;
#endif
          if (voxel > ch10V) {
            rayDepth = currentRayLength;
            ch10V = voxel;
          }
        }
        saturated = saturated && ch10V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_10, samplePos).r;
#else
        voxel = texture3D(volume_10, samplePos).r;
#endif
        chColor = applyTF(transfer_function_10, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 11

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch11Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_11, samplePos).r;
#else
          voxel = texture3D(volume_11, samplePos).r;
#endif
          if (voxel <= ch11V && ch11V >= local_MIP_threshold) {
            ch11Done = true;
          } else if (voxel > ch11V) {
            ch11V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch11Done;
#else
        if (ch11V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_11, samplePos).r;
#else
          voxel = texture3D(volume_11, samplePos).r;
#endif
          if (voxel > ch11V) {
            rayDepth = currentRayLength;
            ch11V = voxel;
          }
        }
        saturated = saturated && ch11V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_11, samplePos).r;
#else
        voxel = texture3D(volume_11, samplePos).r;
#endif
        chColor = applyTF(transfer_function_11, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 12

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch12Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_12, samplePos).r;
#else
          voxel = texture3D(volume_12, samplePos).r;
#endif
          if (voxel <= ch12V && ch12V >= local_MIP_threshold) {
            ch12Done = true;
          } else if (voxel > ch12V) {
            ch12V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch12Done;
#else
        if (ch12V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_12, samplePos).r;
#else
          voxel = texture3D(volume_12, samplePos).r;
#endif
          if (voxel > ch12V) {
            rayDepth = currentRayLength;
            ch12V = voxel;
          }
        }
        saturated = saturated && ch12V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_12, samplePos).r;
#else
        voxel = texture3D(volume_12, samplePos).r;
#endif
        chColor = applyTF(transfer_function_12, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 13

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch13Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_13, samplePos).r;
#else
          voxel = texture3D(volume_13, samplePos).r;
#endif
          if (voxel <= ch13V && ch13V >= local_MIP_threshold) {
            ch13Done = true;
          } else if (voxel > ch13V) {
            ch13V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch13Done;
#else
        if (ch13V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_13, samplePos).r;
#else
          voxel = texture3D(volume_13, samplePos).r;
#endif
          if (voxel > ch13V) {
            rayDepth = currentRayLength;
            ch13V = voxel;
          }
        }
        saturated = saturated && ch13V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_13, samplePos).r;
#else
        voxel = texture3D(volume_13, samplePos).r;
#endif
        chColor = applyTF(transfer_function_13, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 14

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch14Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_14, samplePos).r;
#else
          voxel = texture3D(volume_14, samplePos).r;
#endif
          if (voxel <= ch14V && ch14V >= local_MIP_threshold) {
            ch14Done = true;
          } else if (voxel > ch14V) {
            ch14V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch14Done;
#else
        if (ch14V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_14, samplePos).r;
#else
          voxel = texture3D(volume_14, samplePos).r;
#endif
          if (voxel > ch14V) {
            rayDepth = currentRayLength;
            ch14V = voxel;
          }
        }
        saturated = saturated && ch14V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_14, samplePos).r;
#else
        voxel = texture3D(volume_14, samplePos).r;
#endif
        chColor = applyTF(transfer_function_14, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 15

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch15Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_15, samplePos).r;
#else
          voxel = texture3D(volume_15, samplePos).r;
#endif
          if (voxel <= ch15V && ch15V >= local_MIP_threshold) {
            ch15Done = true;
          } else if (voxel > ch15V) {
            ch15V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch15Done;
#else
        if (ch15V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_15, samplePos).r;
#else
          voxel = texture3D(volume_15, samplePos).r;
#endif
          if (voxel > ch15V) {
            rayDepth = currentRayLength;
            ch15V = voxel;
          }
        }
        saturated = saturated && ch15V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_15, samplePos).r;
#else
        voxel = texture3D(volume_15, samplePos).r;
#endif
        chColor = applyTF(transfer_function_15, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 16

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch16Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_16, samplePos).r;
#else
          voxel = texture3D(volume_16, samplePos).r;
#endif
          if (voxel <= ch16V && ch16V >= local_MIP_threshold) {
            ch16Done = true;
          } else if (voxel > ch16V) {
            ch16V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch16Done;
#else
        if (ch16V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_16, samplePos).r;
#else
          voxel = texture3D(volume_16, samplePos).r;
#endif
          if (voxel > ch16V) {
            rayDepth = currentRayLength;
            ch16V = voxel;
          }
        }
        saturated = saturated && ch16V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_16, samplePos).r;
#else
        voxel = texture3D(volume_16, samplePos).r;
#endif
        chColor = applyTF(transfer_function_16, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 17

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch17Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_17, samplePos).r;
#else
          voxel = texture3D(volume_17, samplePos).r;
#endif
          if (voxel <= ch17V && ch17V >= local_MIP_threshold) {
            ch17Done = true;
          } else if (voxel > ch17V) {
            ch17V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch17Done;
#else
        if (ch17V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_17, samplePos).r;
#else
          voxel = texture3D(volume_17, samplePos).r;
#endif
          if (voxel > ch17V) {
            rayDepth = currentRayLength;
            ch17V = voxel;
          }
        }
        saturated = saturated && ch17V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_17, samplePos).r;
#else
        voxel = texture3D(volume_17, samplePos).r;
#endif
        chColor = applyTF(transfer_function_17, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 18

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch18Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_18, samplePos).r;
#else
          voxel = texture3D(volume_18, samplePos).r;
#endif
          if (voxel <= ch18V && ch18V >= local_MIP_threshold) {
            ch18Done = true;
          } else if (voxel > ch18V) {
            ch18V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch18Done;
#else
        if (ch18V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_18, samplePos).r;
#else
          voxel = texture3D(volume_18, samplePos).r;
#endif
          if (voxel > ch18V) {
            rayDepth = currentRayLength;
            ch18V = voxel;
          }
        }
        saturated = saturated && ch18V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_18, samplePos).r;
#else
        voxel = texture3D(volume_18, samplePos).r;
#endif
        chColor = applyTF(transfer_function_18, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 19

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch19Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_19, samplePos).r;
#else
          voxel = texture3D(volume_19, samplePos).r;
#endif
          if (voxel <= ch19V && ch19V >= local_MIP_threshold) {
            ch19Done = true;
          } else if (voxel > ch19V) {
            ch19V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch19Done;
#else
        if (ch19V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_19, samplePos).r;
#else
          voxel = texture3D(volume_19, samplePos).r;
#endif
          if (voxel > ch19V) {
            rayDepth = currentRayLength;
            ch19V = voxel;
          }
        }
        saturated = saturated && ch19V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_19, samplePos).r;
#else
        voxel = texture3D(volume_19, samplePos).r;
#endif
        chColor = applyTF(transfer_function_19, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 20

#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch20Done) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_20, samplePos).r;
#else
          voxel = texture3D(volume_20, samplePos).r;
#endif
          if (voxel <= ch20V && ch20V >= local_MIP_threshold) {
            ch20Done = true;
          } else if (voxel > ch20V) {
            ch20V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch20Done;
#else
        if (ch20V < 1.0) {
#if GLSL_VERSION >= 130
          voxel = texture(volume_20, samplePos).r;
#else
          voxel = texture3D(volume_20, samplePos).r;
#endif
          if (voxel > ch20V) {
            rayDepth = currentRayLength;
            ch20V = voxel;
          }
        }
        saturated = saturated && ch20V >= 1.0;
#endif
#else
#if GLSL_VERSION >= 130
        voxel = texture(volume_20, samplePos).r;
#else
        voxel = texture3D(volume_20, samplePos).r;
#endif
        chColor = applyTF(transfer_function_20, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


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
        currentRayLength += stepSize;
        finished = finished || (currentRayLength > 1.0);
      }
    }

#ifdef MIP
#if NUM_VOLUMES >= 1
  result = max(result, applyTF(transfer_function_1, ch1V));
#endif
#if NUM_VOLUMES >= 2
  result = max(result, applyTF(transfer_function_2, ch2V));
#endif
#if NUM_VOLUMES >= 3
  result = max(result, applyTF(transfer_function_3, ch3V));
#endif
#if NUM_VOLUMES >= 4
  result = max(result, applyTF(transfer_function_4, ch4V));
#endif
#if NUM_VOLUMES >= 5
  result = max(result, applyTF(transfer_function_5, ch5V));
#endif
#if NUM_VOLUMES >= 6
  result = max(result, applyTF(transfer_function_6, ch6V));
#endif
#if NUM_VOLUMES >= 7
  result = max(result, applyTF(transfer_function_7, ch7V));
#endif
#if NUM_VOLUMES >= 8
  result = max(result, applyTF(transfer_function_8, ch8V));
#endif
#if NUM_VOLUMES >= 9
  result = max(result, applyTF(transfer_function_9, ch9V));
#endif
#if NUM_VOLUMES >= 10
  result = max(result, applyTF(transfer_function_10, ch10V));
#endif
#if NUM_VOLUMES >= 11
  result = max(result, applyTF(transfer_function_11, ch11V));
#endif
#if NUM_VOLUMES >= 12
  result = max(result, applyTF(transfer_function_12, ch12V));
#endif
#if NUM_VOLUMES >= 13
  result = max(result, applyTF(transfer_function_13, ch13V));
#endif
#if NUM_VOLUMES >= 14
  result = max(result, applyTF(transfer_function_14, ch14V));
#endif
#if NUM_VOLUMES >= 15
  result = max(result, applyTF(transfer_function_15, ch15V));
#endif
#if NUM_VOLUMES >= 16
  result = max(result, applyTF(transfer_function_16, ch16V));
#endif
#if NUM_VOLUMES >= 17
  result = max(result, applyTF(transfer_function_17, ch17V));
#endif
#if NUM_VOLUMES >= 18
  result = max(result, applyTF(transfer_function_18, ch18V));
#endif
#if NUM_VOLUMES >= 19
  result = max(result, applyTF(transfer_function_19, ch19V));
#endif
#if NUM_VOLUMES >= 20
  result = max(result, applyTF(transfer_function_20, ch20V));
#endif
#endif // MIP

#ifdef RESULT_OPAQUE
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

