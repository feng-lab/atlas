struct VolumeStruct
{
  sampler2D volume;
};

#if NUM_VOLUMES >= 1
uniform VolumeStruct volume_struct_1;
uniform sampler1D colormap_1;
#endif

#if NUM_VOLUMES >= 2
uniform VolumeStruct volume_struct_2;
uniform sampler1D colormap_2;
#endif

#if NUM_VOLUMES >= 3
uniform VolumeStruct volume_struct_3;
uniform sampler1D colormap_3;
#endif

#if NUM_VOLUMES >= 4
uniform VolumeStruct volume_struct_4;
uniform sampler1D colormap_4;
#endif

#if NUM_VOLUMES >= 5
uniform VolumeStruct volume_struct_5;
uniform sampler1D colormap_5;
#endif

#if NUM_VOLUMES >= 6
uniform VolumeStruct volume_struct_6;
uniform sampler1D colormap_6;
#endif

#if NUM_VOLUMES >= 7
uniform VolumeStruct volume_struct_7;
uniform sampler1D colormap_7;
#endif

#if NUM_VOLUMES >= 8
uniform VolumeStruct volume_struct_8;
uniform sampler1D colormap_8;
#endif

#if NUM_VOLUMES >= 9
uniform VolumeStruct volume_struct_9;
uniform sampler1D colormap_9;
#endif

#if NUM_VOLUMES >= 10
uniform VolumeStruct volume_struct_10;
uniform sampler1D colormap_10;
#endif

#if NUM_VOLUMES >= 11
uniform VolumeStruct volume_struct_11;
uniform sampler1D colormap_11;
#endif

#if NUM_VOLUMES >= 12
uniform VolumeStruct volume_struct_12;
uniform sampler1D colormap_12;
#endif

#if NUM_VOLUMES >= 13
uniform VolumeStruct volume_struct_13;
uniform sampler1D colormap_13;
#endif

#if NUM_VOLUMES >= 14
uniform VolumeStruct volume_struct_14;
uniform sampler1D colormap_14;
#endif

#if NUM_VOLUMES >= 15
uniform VolumeStruct volume_struct_15;
uniform sampler1D colormap_15;
#endif

#if NUM_VOLUMES >= 16
uniform VolumeStruct volume_struct_16;
uniform sampler1D colormap_16;
#endif

#if NUM_VOLUMES >= 17
uniform VolumeStruct volume_struct_17;
uniform sampler1D colormap_17;
#endif

#if NUM_VOLUMES >= 18
uniform VolumeStruct volume_struct_18;
uniform sampler1D colormap_18;
#endif

#if NUM_VOLUMES >= 19
uniform VolumeStruct volume_struct_19;
uniform sampler1D colormap_19;
#endif

#if NUM_VOLUMES >= 20
uniform VolumeStruct volume_struct_20;
uniform sampler1D colormap_20;
#endif

#if GLSL_VERSION >= 130
in vec2 texCoord0;
#else
varying vec2 texCoord0;
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
  voxel = texture2D(volume_struct_1.volume, texCoord0);
#endif
  chColor = applyTF(colormap_1, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 2
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_2.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_2.volume, texCoord0);
#endif
  chColor = applyTF(colormap_2, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 3
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_3.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_3.volume, texCoord0);
#endif
  chColor = applyTF(colormap_3, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 4
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_4.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_4.volume, texCoord0);
#endif
  chColor = applyTF(colormap_4, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 5
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_5.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_5.volume, texCoord0);
#endif
  chColor = applyTF(colormap_5, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 6
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_6.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_6.volume, texCoord0);
#endif
  chColor = applyTF(colormap_6, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 7
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_7.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_7.volume, texCoord0);
#endif
  chColor = applyTF(colormap_7, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 8
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_8.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_8.volume, texCoord0);
#endif
  chColor = applyTF(colormap_8, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 9
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_9.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_9.volume, texCoord0);
#endif
  chColor = applyTF(colormap_9, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 10
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_10.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_10.volume, texCoord0);
#endif
  chColor = applyTF(colormap_10, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 11
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_11.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_11.volume, texCoord0);
#endif
  chColor = applyTF(colormap_11, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 12
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_12.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_12.volume, texCoord0);
#endif
  chColor = applyTF(colormap_12, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 13
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_13.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_13.volume, texCoord0);
#endif
  chColor = applyTF(colormap_13, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 14
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_14.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_14.volume, texCoord0);
#endif
  chColor = applyTF(colormap_14, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 15
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_15.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_15.volume, texCoord0);
#endif
  chColor = applyTF(colormap_15, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 16
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_16.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_16.volume, texCoord0);
#endif
  chColor = applyTF(colormap_16, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 17
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_17.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_17.volume, texCoord0);
#endif
  chColor = applyTF(colormap_17, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 18
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_18.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_18.volume, texCoord0);
#endif
  chColor = applyTF(colormap_18, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 19
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_19.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_19.volume, texCoord0);
#endif
  chColor = applyTF(colormap_19, voxel);
  color = max(color, chColor);
#endif

#if NUM_VOLUMES >= 20
#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_20.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_20.volume, texCoord0);
#endif
  chColor = applyTF(colormap_20, voxel);
  color = max(color, chColor);
#endif

  color.rgb *= color.a;
  FragData0 = color;
#else
  discard;
#endif
}

