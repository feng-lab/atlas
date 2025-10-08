#version 450

// Specialization constants for feature toggles
layout(constant_id = 0) const bool USE_SOFTEDGE = false;
layout(constant_id = 1) const bool SHOW_GLOW    = false;
layout(constant_id = 2) const bool SHOW_OUTLINE = false;
layout(constant_id = 3) const bool SHOW_SHADOW  = false;

// Combined image sampler
layout(set = 0, binding = 0) uniform sampler2D tex;

// Push constants (must match vertex shader declaration/order)
layout(push_constant) uniform PushConstants {
    mat4 projection_view_matrix; // not used here, kept for layout parity
    float alpha;
    float softedge_scale;
    vec2  _pad0;
    vec4  outline_color;
    vec4  shadow_color;
} pc;

// Varyings from vertex
layout(location = 0) in vec2 texCoord0;
layout(location = 1) in vec4 color;

// Output
layout(location = 0) out vec4 FragData0;

void main()
{
    vec4 baseColor = vec4(color.xyz, 1.0);
    float distanceFactor = texture(tex, texCoord0).a;

    if (USE_SOFTEDGE) {
        float width = fwidth(texCoord0.x) * pc.softedge_scale;
        baseColor.a = smoothstep(0.5 - width, 0.5 + width, distanceFactor);
    } else {
        baseColor.a = distanceFactor >= 0.5 ? 1.0 : 0.0;
    }

    if (SHOW_GLOW) {
        const float OUTER_GLOW_MIN = 0.2;
        const float OUTER_GLOW_MAX = 0.5;
        float glowFactor = smoothstep(OUTER_GLOW_MIN, OUTER_GLOW_MAX, distanceFactor);
        baseColor = mix(vec4(pc.outline_color.xyz, glowFactor), baseColor, baseColor.a);
    }

    if (SHOW_OUTLINE) {
        const float OUTLINE_MIN_0 = 0.3;
        float      OUTLINE_MIN_1 = 0.31;
        const float OUTLINE_MAX_0 = 0.5;
        float      OUTLINE_MAX_1 = 0.51;

        if (distanceFactor > OUTLINE_MIN_0 && distanceFactor < OUTLINE_MAX_1) {
            float outlineAlpha;
            if (distanceFactor < OUTLINE_MIN_1)
                outlineAlpha = smoothstep(OUTLINE_MIN_0, OUTLINE_MIN_1, distanceFactor);
            else
                outlineAlpha = smoothstep(OUTLINE_MAX_1, OUTLINE_MAX_0, distanceFactor);

            baseColor = mix(baseColor, pc.outline_color, outlineAlpha);
        }
    }

    if (SHOW_SHADOW) {
        const vec2 GLOW_UV_OFFSET = vec2(-0.0015, -0.0015);
        float glowDistance = texture(tex, texCoord0 + GLOW_UV_OFFSET).a;
        float glowFactor1 = smoothstep(0.3, 0.5, glowDistance);
        baseColor = mix(vec4(pc.shadow_color.xyz, glowFactor1), baseColor, baseColor.a);
    }

    baseColor.a *= pc.alpha;
    baseColor.rgb *= baseColor.a; // premultiply

    FragData0 = baseColor;
}
