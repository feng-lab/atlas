#version 450

layout(location = 0) in vec4 inColor;
layout(location = 1) flat in vec3 fragPlanes[4];
layout(location = 5) flat in vec2 p0pos;
layout(location = 6) flat in vec2 p1pos;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 projectionViewMatrix;
    mat4 modelMatrix;
    float lineWidth;
    float sizeScale;
    int roundCap;
    int screenAlign;
    float alpha;
    bool useLighting;
} pushConstants;

layout(set = 0, binding = 0) uniform sampler1D texSampler;

void main() {
    // Calculate distance to line boundary
    vec3 pos = vec3(gl_FragCoord.xy, 1.0);
    vec4 dist = vec4(
        dot(pos, fragPlanes[0]),
        dot(pos, fragPlanes[1]),
        dot(pos, fragPlanes[2]),
        dot(pos, fragPlanes[3])
    );
    
    // Discard fragments outside the line
    if (pushConstants.roundCap == 1) {
        // For round caps
        if (dist.x < 0 || dist.y < 0) {
            discard;
        }
        if (dist.z < 0 && pushConstants.lineWidth * pushConstants.sizeScale / 2.0 - distance(pos.xy, p0pos) < 0) {
            discard;
        }
        if (dist.w < 0 && pushConstants.lineWidth * pushConstants.sizeScale / 2.0 - distance(pos.xy, p1pos) < 0) {
            discard;
        }
    } else {
        // For square caps
        if (dist.x < 0 || dist.y < 0 || dist.z < 0 || dist.w < 0) {
            discard;
        }
    }
    
    // Calculate smoothing factor based on distance to edge
    float d = min(dist.x, dist.y);
    float f = smoothstep(0.0, 1.0, d);
    
    // Calculate final color
    vec4 finalColor;
    
    // If using texture
    // float texCoord = 1.0 - (d - 1.0) * 2.0 * pushConstants.sizeScale / pushConstants.lineWidth;
    // finalColor = texture(texSampler, texCoord);
    // } else {
    finalColor = inColor;
    // }
    
    // Apply lighting if enabled
    if (pushConstants.useLighting) {
        outColor = vec4(finalColor.rgb * finalColor.a * f * pushConstants.alpha, finalColor.a * f * pushConstants.alpha);
    } else {
        outColor = finalColor;
    }
} 