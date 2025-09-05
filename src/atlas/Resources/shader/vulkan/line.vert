#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 projectionViewMatrix;
    mat4 modelMatrix;
} pushConstants;

void main() {
    vec4 worldPos = pushConstants.modelMatrix * vec4(inPosition, 1.0);
    gl_Position = pushConstants.projectionViewMatrix * worldPos;
    outColor = inColor;
} 