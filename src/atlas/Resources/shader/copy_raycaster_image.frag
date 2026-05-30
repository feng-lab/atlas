uniform sampler2D color_texture;
uniform sampler2D depth_texture;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

#if GLSL_VERSION < 130
#define texelFetch texelFetch2D
#endif

void main()
{
  vec2 rayLengthAndDepth = texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).xy;

  if (rayLengthAndDepth.x < 1.0)
    discard;

  vec4 fragColor = texelFetch(color_texture, ivec2(gl_FragCoord.xy), 0);
  FragData0 = fragColor;

  gl_FragDepth = rayLengthAndDepth.y;
}
