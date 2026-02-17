#version 450

// Vertex attributes
layout(location = 0) in vec3 attr_vertex;
layout(location = 1) in vec2 attr_2dTexCoord0;
layout(location = 2) in vec4 attr_color;

// Varyings to fragment
layout(location = 0) out vec2 texCoord0;
layout(location = 1) out vec4 color;

// Push constants (shared across stages)
layout(push_constant) uniform PushConstants {
    mat4 projection_view_matrix;
    // The following are used by fragment stage but included here to keep a single block layout
    float alpha;
    float softedge_scale;
    vec2  _pad0;
    vec4  outline_color;
    vec4  shadow_color;
    uint  atlas_texture;
    uint  _pad1;
    uint  _pad2;
    uint  _pad3;
} pc;

void main()
{
    gl_Position = pc.projection_view_matrix * vec4(attr_vertex, 1.0);
    texCoord0 = attr_2dTexCoord0;
    color = attr_color;
}
