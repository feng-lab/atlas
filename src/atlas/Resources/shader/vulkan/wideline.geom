#version 450

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

layout(location = 0) in vec4 inColor[];
layout(location = 0) out vec4 outColor;
layout(location = 1) flat out vec3 fragPlanes[4];
layout(location = 5) flat out vec2 p0pos;
layout(location = 6) flat out vec2 p1pos;

layout(push_constant) uniform PushConstants {
    mat4 projectionViewMatrix;
    mat4 modelMatrix;
    float lineWidth;
    float sizeScale;
    int roundCap;
    int screenAlign;
} pushConstants;

// View and projection transformations
uniform mat4 viewportMatrix;
uniform mat4 viewportMatrixInverse;

void main() {
    // Get the clip space line segment
    vec4 p0 = gl_in[0].gl_Position;
    vec4 p1 = gl_in[1].gl_Position;
    
    // Project to screen space
    vec4 sp0 = p0 / p0.w;
    vec4 sp1 = p1 / p1.w;
    
    // Compute segment direction and perpendicular in screen space
    float R = (pushConstants.lineWidth * pushConstants.sizeScale / 2.0 + 1.0);
    vec2 L = normalize(sp1.xy - sp0.xy);
    vec2 P = vec2(-L.y, L.x);
    
    // Determine if we need screen alignment
    vec2 LR = (pushConstants.screenAlign == 1) ? vec2(0, 0) : L * R;
    vec2 PR = P * R;
    
    // For screen alignment, adjust PR to be orthogonal to major axis
    if (pushConstants.screenAlign == 1) {
        vec2 Lmajor = abs(L.y) >= abs(L.x) ? vec2(0, sign(L.y)) : vec2(sign(L.x), 0);
        PR -= dot(PR, Lmajor) / dot(L, Lmajor) * L;
    }
    
    // Compute the quad corners
    vec2 q0 = sp0.xy - LR - PR;
    vec2 q1 = sp1.xy + LR - PR;
    vec2 q2 = sp1.xy + LR + PR;
    vec2 q3 = sp0.xy - LR + PR;
    
    // Convert back to clip space
    vec4 v0 = vec4(q0, sp0.z, 1.0) * p0.w;
    vec4 v1 = vec4(q1, sp1.z, 1.0) * p1.w;
    vec4 v2 = vec4(q2, sp1.z, 1.0) * p1.w;
    vec4 v3 = vec4(q3, sp0.z, 1.0) * p0.w;
    
    // Compute screen-space planes for fragment distance calculations
    vec3 planes[4];
    planes[0] = vec3(+P, -(dot(sp0.xy, +P) - R));
    planes[1] = vec3(-P, -(dot(sp1.xy, -P) - R));
    
    if (pushConstants.screenAlign == 1) {
        vec2 Lmajor = abs(L.y) >= abs(L.x) ? vec2(0, sign(L.y)) : vec2(sign(L.x), 0);
        planes[2] = vec3(+Lmajor, -(dot(sp0.xy, +Lmajor) - 1.0));
        planes[3] = vec3(-Lmajor, -(dot(sp1.xy, -Lmajor) - 1.0));
    } else {
        planes[2] = vec3(+L, -(dot(sp0.xy, +L) - 1.0));
        planes[3] = vec3(-L, -(dot(sp1.xy, -L) - 1.0));
    }
    
    // First vertex
    gl_Position = v0;
    outColor = inColor[0];
    fragPlanes = planes;
    if (pushConstants.roundCap == 1) {
        p0pos = sp0.xy;
        p1pos = sp1.xy;
    }
    EmitVertex();
    
    // Second vertex
    gl_Position = v3;
    outColor = inColor[0];
    fragPlanes = planes;
    if (pushConstants.roundCap == 1) {
        p0pos = sp0.xy;
        p1pos = sp1.xy;
    }
    EmitVertex();
    
    // Third vertex
    gl_Position = v1;
    outColor = inColor[1];
    fragPlanes = planes;
    if (pushConstants.roundCap == 1) {
        p0pos = sp0.xy;
        p1pos = sp1.xy;
    }
    EmitVertex();
    
    // Fourth vertex
    gl_Position = v2;
    outColor = inColor[1];
    fragPlanes = planes;
    if (pushConstants.roundCap == 1) {
        p0pos = sp0.xy;
        p1pos = sp1.xy;
    }
    EmitVertex();
    
    EndPrimitive();
} 