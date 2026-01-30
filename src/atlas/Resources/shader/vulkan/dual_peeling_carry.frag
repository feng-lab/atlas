#version 450

// Fullscreen carry pass for Vulkan DDP:
// - Restores the previous ping's front blender into the current ping so a
//   skipped geometry peel (indirect-count gating) doesn't erase accumulated
//   front color after the per-pass clear.
// - Resets the current ping depth blender to (-1, -1) (done) and clears the
//   back temp to zero.

layout(set = 0, binding = 0) uniform sampler2D DepthBlenderPrevTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderPrevTex;

layout(location = 0) out vec4 FragData0; // depth blender (RG32F)
layout(location = 1) out vec4 FragData1; // front blender (RGBA16)
layout(location = 2) out vec4 FragData2; // back temp (RGBA16)

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  FragData0 = vec4(-1.0, -1.0, 0.0, 0.0);
  FragData1 = texelFetch(FrontBlenderPrevTex, p, 0);
  FragData2 = vec4(0.0);
}

