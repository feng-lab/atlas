uniform vec2 screen_dim_RCP;

uniform sampler2D color_texture;
uniform sampler2D depth_texture;
uniform sampler2D glowmap_color_texture;
uniform sampler2D glowmap_depth_texture;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

#if GLSL_VERSION < 130
#define texture texture2D
#endif

void main()
{
  vec2 texCoords = gl_FragCoord.xy * screen_dim_RCP;

  vec4 dst = texture(color_texture, texCoords);
  vec4 src = texture(glowmap_color_texture, texCoords);

#if defined(ADDITIVE_BLENDING)
  FragData0 = min(src + dst, 1.0);
  if (FragData0.a > 0) {
    float depth = texture(depth_texture, texCoords).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, texCoords).r;
  }
#elif defined(SCREEN_BLENDING)
  FragData0 = clamp((src + dst) - (src * dst), 0.0, 1.0);
  if (FragData0.a > 0) {
    float depth = texture(depth_texture, texCoords).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, texCoords).r;
  }
#elif defined(SOFTLIGHT_BLENDING)
  src = (src * 0.5) + 0.5;
  FragData0.xyz = vec3((src.x <= 0.5) ? (dst.x - (1.0 - 2.0 * src.x) * dst.x * (1.0 - dst.x)) : (((src.x > 0.5) && (dst.x <= 0.25)) ? (dst.x + (2.0 * src.x - 1.0) * (4.0 * dst.x * (4.0 * dst.x + 1.0) * (dst.x - 1.0) + 7.0 * dst.x)) : (dst.x + (2.0 * src.x - 1.0) * (sqrt(dst.x) - dst.x))),
                          (src.y <= 0.5) ? (dst.y - (1.0 - 2.0 * src.y) * dst.y * (1.0 - dst.y)) : (((src.y > 0.5) && (dst.y <= 0.25)) ? (dst.y + (2.0 * src.y - 1.0) * (4.0 * dst.y * (4.0 * dst.y + 1.0) * (dst.y - 1.0) + 7.0 * dst.y)) : (dst.y + (2.0 * src.y - 1.0) * (sqrt(dst.y) - dst.y))),
                          (src.z <= 0.5) ? (dst.z - (1.0 - 2.0 * src.z) * dst.z * (1.0 - dst.z)) : (((src.z > 0.5) && (dst.z <= 0.25)) ? (dst.z + (2.0 * src.z - 1.0) * (4.0 * dst.z * (4.0 * dst.z + 1.0) * (dst.z - 1.0) + 7.0 * dst.z)) : (dst.z + (2.0 * src.z - 1.0) * (sqrt(dst.z) - dst.z))));
  FragData0.a = (src.a + dst.a) * 0.5;
  if (FragData0.a > 0) {
    float depth = texture(depth_texture, texCoords).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, texCoords).r;
  }
#elif defined(GLOWMAP)
  FragData0 = src;
  gl_FragDepth = texture(glowmap_depth_texture, texCoords).r;
#endif
}
