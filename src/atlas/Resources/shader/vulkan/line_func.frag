#version 450

layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    float alpha;
    bool useTexture;
} fragConstants;

void main() {
    outColor = inColor;
    outColor.a *= fragConstants.alpha;
} 