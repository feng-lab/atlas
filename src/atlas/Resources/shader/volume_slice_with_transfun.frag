struct VolumeStruct
{
  sampler3D volume;
};

#if NUM_VOLUMES >= 1
uniform VolumeStruct volume_struct_1;
uniform sampler1D transfer_function_1;
#endif

#if NUM_VOLUMES >= 2
uniform VolumeStruct volume_struct_2;
uniform sampler1D transfer_function_2;
#endif

#if NUM_VOLUMES >= 3
uniform VolumeStruct volume_struct_3;
uniform sampler1D transfer_function_3;
#endif

#if NUM_VOLUMES >= 4
uniform VolumeStruct volume_struct_4;
uniform sampler1D transfer_function_4;
#endif

#if NUM_VOLUMES >= 5
uniform VolumeStruct volume_struct_5;
uniform sampler1D transfer_function_5;
#endif

#if NUM_VOLUMES >= 6
uniform VolumeStruct volume_struct_6;
uniform sampler1D transfer_function_6;
#endif

#if NUM_VOLUMES >= 7
uniform VolumeStruct volume_struct_7;
uniform sampler1D transfer_function_7;
#endif

#if NUM_VOLUMES >= 8
uniform VolumeStruct volume_struct_8;
uniform sampler1D transfer_function_8;
#endif

#if NUM_VOLUMES >= 9
uniform VolumeStruct volume_struct_9;
uniform sampler1D transfer_function_9;
#endif

#if NUM_VOLUMES >= 10
uniform VolumeStruct volume_struct_10;
uniform sampler1D transfer_function_10;
#endif

#if NUM_VOLUMES >= 11
uniform VolumeStruct volume_struct_11;
uniform sampler1D transfer_function_11;
#endif

#if NUM_VOLUMES >= 12
uniform VolumeStruct volume_struct_12;
uniform sampler1D transfer_function_12;
#endif

#if NUM_VOLUMES >= 13
uniform VolumeStruct volume_struct_13;
uniform sampler1D transfer_function_13;
#endif

#if NUM_VOLUMES >= 14
uniform VolumeStruct volume_struct_14;
uniform sampler1D transfer_function_14;
#endif

#if NUM_VOLUMES >= 15
uniform VolumeStruct volume_struct_15;
uniform sampler1D transfer_function_15;
#endif

#if NUM_VOLUMES >= 16
uniform VolumeStruct volume_struct_16;
uniform sampler1D transfer_function_16;
#endif

#if NUM_VOLUMES >= 17
uniform VolumeStruct volume_struct_17;
uniform sampler1D transfer_function_17;
#endif

#if NUM_VOLUMES >= 18
uniform VolumeStruct volume_struct_18;
uniform sampler1D transfer_function_18;
#endif

#if NUM_VOLUMES >= 19
uniform VolumeStruct volume_struct_19;
uniform sampler1D transfer_function_19;
#endif

#if NUM_VOLUMES >= 20
uniform VolumeStruct volume_struct_20;
uniform sampler1D transfer_function_20;
#endif

#if GLSL_VERSION >= 130
in vec3 texCoord0;
#else
varying vec3 texCoord0;
#endif

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

vec4 applyTF(in sampler1D tex, in vec4 intensity)
{
#if GLSL_VERSION >= 130
  return texture(tex, intensity.r);
#else
  return texture1D(tex, intensity.r);
#endif
}

void main()
{
#if NUM_VOLUMES > 0
  vec4 color = vec4(0.0);
  vec4 voxel;
  vec4 chColor;

#if NUM_VOLUMES >= 1
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_1.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_1.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_1, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 2
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_2.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_2.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_2, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 3
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_3.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_3.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_3, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 4
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_4.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_4.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_4, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 5
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_5.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_5.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_5, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 6
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_6.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_6.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_6, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 7
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_7.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_7.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_7, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 8
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_8.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_8.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_8, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 9
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_9.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_9.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_9, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 10
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_10.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_10.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_10, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 11
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_11.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_11.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_11, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 12
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_12.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_12.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_12, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 13
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_13.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_13.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_13, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 14
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_14.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_14.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_14, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 15
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_15.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_15.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_15, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 16
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_16.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_16.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_16, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 17
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_17.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_17.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_17, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 18
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_18.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_18.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_18, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 19
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_19.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_19.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_19, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#if NUM_VOLUMES >= 20
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_20.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_20.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_20, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }
#endif

#ifdef RESULT_OPAQUE
  color.a = 1.0;
#else
  if (color.a == 0.0)
    discard;
#endif

  color.rgb *= color.a;
  FragData0 = color;
#else
  discard;
#endif
}

