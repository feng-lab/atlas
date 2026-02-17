#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(constant_id = 51) const bool RESULT_OPAQUE = false;

layout(std140, set = 1, binding = 0) uniform Image2DSingleBindlessUBO {
  uint volume_1;
  uint transfer_function_1;
  uint _pad0;
  uint _pad1;
} sbubo;

layout(location = 0) in vec2 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  vec4 color =
    texture(atlas_bindlessSampler2DLinear(sbubo.transfer_function_1),
            vec2(texture(atlas_bindlessSampler2DLinear(sbubo.volume_1), texCoord0).r, 0.5));
  if (color.a == 0.0) {
    color = vec4(0.0);
  }
  if (RESULT_OPAQUE) {
    color.a = 1.0;
  } else {
    color.rgb *= color.a;
  }
  FragData0 = color;
}
