#version 450

// Thin-line DDP init.
// Outputs the initial min/max depth range into the depth blender and stores
// -minDepth into depthTex for the final composite pass.
//
// Uses device depth (gl_FragCoord.z); no additional stage inputs required.

layout(location = 0) out vec4 FragData0; // depth blender (RG32F in the DDP RT)
layout(location = 1) out vec4 FragData1; // depthTex (R32F in the DDP RT)

void main()
{
  const float fragDepth = gl_FragCoord.z;
  FragData0.xy = vec2(-fragDepth, fragDepth);
  FragData1.x  = -fragDepth;
}

