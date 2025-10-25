#version 450

layout(set = 0, binding = 0) uniform sampler2D TempTex;


layout(location = 0) out vec4 FragData0;

void main()
{
  FragData0 = texelFetch(TempTex, ivec2(gl_FragCoord.xy), 0);
  if (FragData0.a == 0.0) discard; // for occlusion query
}
