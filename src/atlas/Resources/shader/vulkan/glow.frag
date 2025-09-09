#version 450

// Modes: 0=ADDITIVE, 1=SCREEN, 2=SOFTLIGHT, 3=GLOWMAP
layout(constant_id = 110) const int GLOW_MODE = 0;

layout(set = 0, binding = 0) uniform sampler2D color_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D glowmap_color_texture;
layout(set = 0, binding = 3) uniform sampler2D glowmap_depth_texture;

layout(push_constant) uniform GlowPC {
  vec2 screen_dim_RCP;
} pc;

layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 tc = gl_FragCoord.xy * pc.screen_dim_RCP;
  vec4 dst = texture(color_texture, tc);
  vec4 src = texture(glowmap_color_texture, tc);
  if (src.a == 0.0 && dst.a == 0.0) discard;

  if (GLOW_MODE == 0) { // ADDITIVE_BLENDING
    FragData0 = min(src + dst, 1.0);
    float depth = texture(depth_texture, tc).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, tc).r;
  } else if (GLOW_MODE == 1) { // SCREEN_BLENDING
    FragData0 = clamp((src + dst) - (src * dst), 0.0, 1.0);
    float depth = texture(depth_texture, tc).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, tc).r;
  } else if (GLOW_MODE == 2) { // SOFTLIGHT_BLENDING
    src = (src * 0.5) + 0.5;
    FragData0 = vec4((src.x <= 0.5) ? (dst.x - (1.0 - 2.0 * src.x) * dst.x * (1.0 - dst.x)) : (((src.x > 0.5) && (dst.x <= 0.25)) ? (dst.x + (2.0 * src.x - 1.0) * (4.0 * dst.x * (4.0 * dst.x + 1.0) * (dst.x - 1.0) + 7.0 * dst.x)) : (dst.x + (2.0 * src.x - 1.0) * (sqrt(dst.x) - dst.x))),
                          (src.y <= 0.5) ? (dst.y - (1.0 - 2.0 * src.y) * dst.y * (1.0 - dst.y)) : (((src.y > 0.5) && (dst.y <= 0.25)) ? (dst.y + (2.0 * src.y - 1.0) * (4.0 * dst.y * (4.0 * dst.y + 1.0) * (dst.y - 1.0) + 7.0 * dst.y)) : (dst.y + (2.0 * src.y - 1.0) * (sqrt(dst.y) - dst.y))),
                          (src.z <= 0.5) ? (dst.z - (1.0 - 2.0 * src.z) * dst.z * (1.0 - dst.z)) : (((src.z > 0.5) && (dst.z <= 0.25)) ? (dst.z + (2.0 * src.z - 1.0) * (4.0 * dst.z * (4.0 * dst.z + 1.0) * (dst.z - 1.0) + 7.0 * dst.z)) : (dst.z + (2.0 * src.z - 1.0) * (sqrt(dst.z) - dst.z))),
                          (src.w <= 0.5) ? (dst.w - (1.0 - 2.0 * src.w) * dst.w * (1.0 - dst.w)) : (((src.w > 0.5) && (dst.w <= 0.25)) ? (dst.w + (2.0 * src.w - 1.0) * (4.0 * dst.w * (4.0 * dst.w + 1.0) * (dst.w - 1.0) + 7.0 * dst.w)) : (dst.w + (2.0 * src.w - 1.0) * (sqrt(dst.w) - dst.w))));
    float depth = texture(depth_texture, tc).r;
    gl_FragDepth = depth < 1.0 ? depth : texture(glowmap_depth_texture, tc).r;
  } else { // GLOWMAP
    FragData0 = src;
    gl_FragDepth = texture(glowmap_depth_texture, tc).r;
  }
}

