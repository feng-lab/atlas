#version 450

layout(location = 0) in vec3 inP0;
layout(location = 1) in vec3 inP1;
layout(location = 2) in float inFlag;
layout(location = 3) in vec4 inP0Color;
layout(location = 4) in vec4 inP1Color;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 projectionViewMatrix;
    mat4 modelMatrix;
    float lineWidth;
    int roundCap;
    int screenAlign;
    float padding;
} pushConstants;

void main() {
    // Transform points to world space
    vec4 p0 = pushConstants.modelMatrix * vec4(inP0, 1.0);
    vec4 p1 = pushConstants.modelMatrix * vec4(inP1, 1.0);
    
    // Project to normalized device coordinates
    vec4 projP0 = pushConstants.projectionViewMatrix * p0;
    vec4 projP1 = pushConstants.projectionViewMatrix * p1;
    
    // Perform perspective division
    vec3 ndcP0 = projP0.xyz / projP0.w;
    vec3 ndcP1 = projP1.xyz / projP1.w;
    
    // Calculate line direction and perpendicular in NDC
    vec2 lineDir = normalize(ndcP1.xy - ndcP0.xy);
    vec2 perpDir = vec2(-lineDir.y, lineDir.x);
    
    // Calculate half width in NDC space (adjusted for aspect ratio if needed)
    float halfWidth = pushConstants.lineWidth * 0.5;
    
    // Adjust for screen alignment if needed
    if (pushConstants.screenAlign == 1) {
        vec2 majorAxis = abs(lineDir.y) > abs(lineDir.x) ? 
                         vec2(0.0, sign(lineDir.y)) : 
                         vec2(sign(lineDir.x), 0.0);
        perpDir = perpDir - dot(perpDir, majorAxis) / dot(lineDir, majorAxis) * lineDir;
        perpDir = normalize(perpDir);
    }
    
    // Create quad vertex based on flag
    vec2 offset;
    if (inFlag < 0.0) {
        // Left side vertices
        offset = perpDir * (-halfWidth);
        outColor = inP0Color;
    } else {
        // Right side vertices
        offset = perpDir * halfWidth;
        outColor = inP1Color;
    }
    
    // Determine which endpoint to use
    float t = (abs(inFlag) - 1.0) * 0.5; // Maps -3,-1 to 1,0 and 1,3 to 0,1
    vec3 pos = mix(ndcP0, ndcP1, t);
    
    // Add offset perpendicular to line
    pos.xy += offset;
    
    // Convert back to clip space
    float w = mix(projP0.w, projP1.w, t);
    gl_Position = vec4(pos * w, w);
} 