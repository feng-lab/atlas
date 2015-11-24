struct VolumeStruct
{
  sampler2D volume;
};

uniform VolumeStruct volume_struct_1;
uniform sampler1D transfer_function_1;

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

#if GLSL_VERSION >= 130
  voxel = texture(volume_struct_1.volume, texCoord0);
#else
  voxel = texture2D(volume_struct_1.volume, texCoord0);
#endif
  chColor = applyTF(transfer_function_1, voxel);
  if (chColor.a > 0.0) {
    color = max(color, chColor);
  }

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

