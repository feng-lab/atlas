struct VolumeStruct
{
  sampler3D volume;
  vec3 dimensions;
};

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
uniform VolumeStruct volume_struct_1;
uniform TF_SAMPLER_TYPE_1 transfer_function_1;
#endif

#if NUM_VOLUMES >= 2
uniform VolumeStruct volume_struct_2;
uniform TF_SAMPLER_TYPE_2 transfer_function_2;
#endif

#if NUM_VOLUMES >= 3
uniform VolumeStruct volume_struct_3;
uniform TF_SAMPLER_TYPE_3 transfer_function_3;
#endif

#if NUM_VOLUMES >= 4
uniform VolumeStruct volume_struct_4;
uniform TF_SAMPLER_TYPE_4 transfer_function_4;
#endif

#if NUM_VOLUMES >= 5
uniform VolumeStruct volume_struct_5;
uniform TF_SAMPLER_TYPE_5 transfer_function_5;
#endif

#if NUM_VOLUMES >= 6
uniform VolumeStruct volume_struct_6;
uniform TF_SAMPLER_TYPE_6 transfer_function_6;
#endif

#if NUM_VOLUMES >= 7
uniform VolumeStruct volume_struct_7;
uniform TF_SAMPLER_TYPE_7 transfer_function_7;
#endif

#if NUM_VOLUMES >= 8
uniform VolumeStruct volume_struct_8;
uniform TF_SAMPLER_TYPE_8 transfer_function_8;
#endif

#if NUM_VOLUMES >= 9
uniform VolumeStruct volume_struct_9;
uniform TF_SAMPLER_TYPE_9 transfer_function_9;
#endif

#if NUM_VOLUMES >= 10
uniform VolumeStruct volume_struct_10;
uniform TF_SAMPLER_TYPE_10 transfer_function_10;
#endif

#if NUM_VOLUMES >= 11
uniform VolumeStruct volume_struct_11;
uniform TF_SAMPLER_TYPE_11 transfer_function_11;
#endif

#if NUM_VOLUMES >= 12
uniform VolumeStruct volume_struct_12;
uniform TF_SAMPLER_TYPE_12 transfer_function_12;
#endif

#if NUM_VOLUMES >= 13
uniform VolumeStruct volume_struct_13;
uniform TF_SAMPLER_TYPE_13 transfer_function_13;
#endif

#if NUM_VOLUMES >= 14
uniform VolumeStruct volume_struct_14;
uniform TF_SAMPLER_TYPE_14 transfer_function_14;
#endif

#if NUM_VOLUMES >= 15
uniform VolumeStruct volume_struct_15;
uniform TF_SAMPLER_TYPE_15 transfer_function_15;
#endif

#if NUM_VOLUMES >= 16
uniform VolumeStruct volume_struct_16;
uniform TF_SAMPLER_TYPE_16 transfer_function_16;
#endif

#if NUM_VOLUMES >= 17
uniform VolumeStruct volume_struct_17;
uniform TF_SAMPLER_TYPE_17 transfer_function_17;
#endif

#if NUM_VOLUMES >= 18
uniform VolumeStruct volume_struct_18;
uniform TF_SAMPLER_TYPE_18 transfer_function_18;
#endif

#if NUM_VOLUMES >= 19
uniform VolumeStruct volume_struct_19;
uniform TF_SAMPLER_TYPE_19 transfer_function_19;
#endif

#if NUM_VOLUMES >= 20
uniform VolumeStruct volume_struct_20;
uniform TF_SAMPLER_TYPE_20 transfer_function_20;
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

    vec3 dimension = volume_struct_1.dimensions;
    vec3 rayVector = exitRayPosition - startRayPosition;
    float maxRayLength = length(rayVector);
    float stepSize = maxRayLength / (sampling_rate * length(normalize(rayVector) * dimension));

    float currentRayLength = 0.0;
    float rayDepth = -1.0;
    bool finished = false;
    for (int loop0=0; !finished && loop0<255; loop0++) {
      for (int loop1=0; !finished && loop1<255; loop1++) {
        float voxel;
        vec4 color = vec4(0.0);
        vec4 chColor;
        vec3 samplePos = startRayPosition + currentRayLength / maxRayLength * rayVector;
        bool saturated = true;

#if NUM_VOLUMES >= 1

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_1.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_1.volume, samplePos).r;
#endif
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

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 2

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_2.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_2.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch2Done) {
          if (voxel <= ch2V && ch2V >= local_MIP_threshold) {
            ch2Done = true;
          } else if (voxel > ch2V) {
            ch2V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch2Done;
#else
        if (voxel > ch2V) {
          rayDepth = currentRayLength;
          ch2V = voxel;
        }
        saturated = saturated && ch2V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_2, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 3

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_3.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_3.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch3Done) {
          if (voxel <= ch3V && ch3V >= local_MIP_threshold) {
            ch3Done = true;
          } else if (voxel > ch3V) {
            ch3V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch3Done;
#else
        if (voxel > ch3V) {
          rayDepth = currentRayLength;
          ch3V = voxel;
        }
        saturated = saturated && ch3V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_3, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 4

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_4.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_4.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch4Done) {
          if (voxel <= ch4V && ch4V >= local_MIP_threshold) {
            ch4Done = true;
          } else if (voxel > ch4V) {
            ch4V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch4Done;
#else
        if (voxel > ch4V) {
          rayDepth = currentRayLength;
          ch4V = voxel;
        }
        saturated = saturated && ch4V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_4, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 5

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_5.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_5.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch5Done) {
          if (voxel <= ch5V && ch5V >= local_MIP_threshold) {
            ch5Done = true;
          } else if (voxel > ch5V) {
            ch5V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch5Done;
#else
        if (voxel > ch5V) {
          rayDepth = currentRayLength;
          ch5V = voxel;
        }
        saturated = saturated && ch5V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_5, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 6

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_6.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_6.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch6Done) {
          if (voxel <= ch6V && ch6V >= local_MIP_threshold) {
            ch6Done = true;
          } else if (voxel > ch6V) {
            ch6V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch6Done;
#else
        if (voxel > ch6V) {
          rayDepth = currentRayLength;
          ch6V = voxel;
        }
        saturated = saturated && ch6V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_6, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 7

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_7.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_7.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch7Done) {
          if (voxel <= ch7V && ch7V >= local_MIP_threshold) {
            ch7Done = true;
          } else if (voxel > ch7V) {
            ch7V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch7Done;
#else
        if (voxel > ch7V) {
          rayDepth = currentRayLength;
          ch7V = voxel;
        }
        saturated = saturated && ch7V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_7, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 8

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_8.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_8.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch8Done) {
          if (voxel <= ch8V && ch8V >= local_MIP_threshold) {
            ch8Done = true;
          } else if (voxel > ch8V) {
            ch8V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch8Done;
#else
        if (voxel > ch8V) {
          rayDepth = currentRayLength;
          ch8V = voxel;
        }
        saturated = saturated && ch8V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_8, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 9

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_9.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_9.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch9Done) {
          if (voxel <= ch9V && ch9V >= local_MIP_threshold) {
            ch9Done = true;
          } else if (voxel > ch9V) {
            ch9V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch9Done;
#else
        if (voxel > ch9V) {
          rayDepth = currentRayLength;
          ch9V = voxel;
        }
        saturated = saturated && ch9V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_9, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 10

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_10.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_10.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch10Done) {
          if (voxel <= ch10V && ch10V >= local_MIP_threshold) {
            ch10Done = true;
          } else if (voxel > ch10V) {
            ch10V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch10Done;
#else
        if (voxel > ch10V) {
          rayDepth = currentRayLength;
          ch10V = voxel;
        }
        saturated = saturated && ch10V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_10, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 11

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_11.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_11.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch11Done) {
          if (voxel <= ch11V && ch11V >= local_MIP_threshold) {
            ch11Done = true;
          } else if (voxel > ch11V) {
            ch11V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch11Done;
#else
        if (voxel > ch11V) {
          rayDepth = currentRayLength;
          ch11V = voxel;
        }
        saturated = saturated && ch11V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_11, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 12

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_12.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_12.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch12Done) {
          if (voxel <= ch12V && ch12V >= local_MIP_threshold) {
            ch12Done = true;
          } else if (voxel > ch12V) {
            ch12V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch12Done;
#else
        if (voxel > ch12V) {
          rayDepth = currentRayLength;
          ch12V = voxel;
        }
        saturated = saturated && ch12V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_12, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 13

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_13.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_13.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch13Done) {
          if (voxel <= ch13V && ch13V >= local_MIP_threshold) {
            ch13Done = true;
          } else if (voxel > ch13V) {
            ch13V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch13Done;
#else
        if (voxel > ch13V) {
          rayDepth = currentRayLength;
          ch13V = voxel;
        }
        saturated = saturated && ch13V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_13, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 14

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_14.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_14.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch14Done) {
          if (voxel <= ch14V && ch14V >= local_MIP_threshold) {
            ch14Done = true;
          } else if (voxel > ch14V) {
            ch14V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch14Done;
#else
        if (voxel > ch14V) {
          rayDepth = currentRayLength;
          ch14V = voxel;
        }
        saturated = saturated && ch14V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_14, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 15

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_15.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_15.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch15Done) {
          if (voxel <= ch15V && ch15V >= local_MIP_threshold) {
            ch15Done = true;
          } else if (voxel > ch15V) {
            ch15V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch15Done;
#else
        if (voxel > ch15V) {
          rayDepth = currentRayLength;
          ch15V = voxel;
        }
        saturated = saturated && ch15V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_15, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 16

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_16.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_16.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch16Done) {
          if (voxel <= ch16V && ch16V >= local_MIP_threshold) {
            ch16Done = true;
          } else if (voxel > ch16V) {
            ch16V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch16Done;
#else
        if (voxel > ch16V) {
          rayDepth = currentRayLength;
          ch16V = voxel;
        }
        saturated = saturated && ch16V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_16, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 17

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_17.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_17.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch17Done) {
          if (voxel <= ch17V && ch17V >= local_MIP_threshold) {
            ch17Done = true;
          } else if (voxel > ch17V) {
            ch17V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch17Done;
#else
        if (voxel > ch17V) {
          rayDepth = currentRayLength;
          ch17V = voxel;
        }
        saturated = saturated && ch17V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_17, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 18

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_18.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_18.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch18Done) {
          if (voxel <= ch18V && ch18V >= local_MIP_threshold) {
            ch18Done = true;
          } else if (voxel > ch18V) {
            ch18V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch18Done;
#else
        if (voxel > ch18V) {
          rayDepth = currentRayLength;
          ch18V = voxel;
        }
        saturated = saturated && ch18V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_18, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 19

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_19.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_19.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch19Done) {
          if (voxel <= ch19V && ch19V >= local_MIP_threshold) {
            ch19Done = true;
          } else if (voxel > ch19V) {
            ch19V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch19Done;
#else
        if (voxel > ch19V) {
          rayDepth = currentRayLength;
          ch19V = voxel;
        }
        saturated = saturated && ch19V >= 1.0;
#endif
#else
        chColor = applyTF(transfer_function_19, voxel);

        if (chColor.a > 0.0) {
          color = max(color, chColor);
        }
#endif //MIP

#endif


#if NUM_VOLUMES >= 20

#if GLSL_VERSION >= 130
        voxel = texture(volume_struct_20.volume, samplePos).r;
#else
        voxel = texture3D(volume_struct_20.volume, samplePos).r;
#endif
#ifdef MIP
#ifdef LOCAL_MIP
        if (!ch20Done) {
          if (voxel <= ch20V && ch20V >= local_MIP_threshold) {
            ch20Done = true;
          } else if (voxel > ch20V) {
            ch20V = voxel;
            rayDepth = currentRayLength;
          }
        }
        saturated = saturated && ch20Done;
#else
        if (voxel > ch20V) {
          rayDepth = currentRayLength;
          ch20V = voxel;
        }
        saturated = saturated && ch20V >= 1.0;
#endif
#else
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
        finished = finished || (currentRayLength > maxRayLength);
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
      float ze = zeFront + rayDepth / maxRayLength * (zeBack-zeFront);
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

