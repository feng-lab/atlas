#version 450

layout(set = 0, binding = 0) uniform sampler3D volume_1;
layout(set = 0, binding = 1) uniform sampler1D colormap_1;

layout(location = 0) in vec3 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  vec4 c = texture(colormap_1, texture(volume_1, texCoord0).r);
  c.rgb *= c.a;
  FragData0 = c;
}
