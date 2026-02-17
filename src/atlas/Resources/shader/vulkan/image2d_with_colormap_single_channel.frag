#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(std140, set = 1, binding = 0) uniform Image2DSingleBindlessUBO {
  uint volume_1;
  uint colormap_1;
  uint _pad0;
  uint _pad1;
} sbubo;

layout(location = 0) in vec2 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  vec4 c = texture(atlas_bindlessSampler2DLinear(sbubo.colormap_1),
                   vec2(texture(atlas_bindlessSampler2DLinear(sbubo.volume_1), texCoord0).r, 0.5));
  c.rgb *= c.a;
  FragData0 = c;
}
