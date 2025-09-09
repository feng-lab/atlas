#version 450

layout(constant_id = 50) const bool VALID_INPUT = true;

layout(set = 0, binding = 0) uniform sampler2D volume_1;
layout(set = 0, binding = 1) uniform sampler1D colormap_1;

layout(location = 0) in vec2 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  if (!VALID_INPUT) discard;
  vec4 c = texture(colormap_1, texture(volume_1, texCoord0).r);
  c.rgb *= c.a;
  FragData0 = c;
}

