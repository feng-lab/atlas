struct VolumeStruct
{
  sampler3D volume;
};

uniform VolumeStruct volume_struct_1;
uniform sampler1D colormap_1;

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

#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_1.volume, texCoord0);
#else
  voxel = texture3D(volume_struct_1.volume, texCoord0);
#endif
  chColor = applyTF(colormap_1, voxel);
  color = max(color, chColor);

  color.rgb *= color.a;
  FragData0 = color;
#else
  discard;
#endif
}

