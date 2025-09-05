#version 450

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 projectionViewMatrix;
    mat4 modelMatrix;
    float lineWidth;
    int roundCap;
    int screenAlign;
    float padding;
    float alpha;
    bool useLighting;
} pushConstants;

layout(set = 0, binding = 0) uniform sampler1D texSampler;

void main() {
    // For the non-geometry shader approach, we've already constructed the quads
    // in the vertex shader, so we can just use the color directly
    
    vec4 finalColor = inColor;
    
    // Apply lighting if enabled
    if (pushConstants.useLighting) {
        outColor = vec4(finalColor.rgb * pushConstants.alpha, finalColor.a * pushConstants.alpha);
    } else {
        outColor = finalColor;
        outColor.a *= pushConstants.alpha;
    }
} 