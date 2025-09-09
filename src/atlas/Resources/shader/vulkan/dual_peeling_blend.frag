#version 450

layout(set = 0, binding = 0) uniform sampler2D TempTex;

#include "include/oit_params.glslinc"

layout(location = 0) out vec4 FragData0;

void main()
{
  FragData0 = texture(TempTex, gl_FragCoord.xy * oit.screen_dim_RCP);
  if (FragData0.a == 0.0) discard; // for occlusion query
}
