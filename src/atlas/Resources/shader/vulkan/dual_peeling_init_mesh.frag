#version 450

// Mesh DDP init only needs device depth (gl_FragCoord.z). Avoid pulling in the
// full mesh shading path so the fragment shader has no stage inputs after -O,
// which matches the depth-only vertex shader used by the Vulkan backend.

layout(location = 0) out vec4 FragData0;
layout(location = 1) out vec4 FragData1;

void main()
{
  const float fragDepth = gl_FragCoord.z;
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}
