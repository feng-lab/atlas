#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 60) const bool DISCARD_TRANSPARENT = false;
layout(constant_id = 61) const bool MULTIPLY_ALPHA      = false;
layout(constant_id = 62) const bool DIVIDE_BY_ALPHA     = false;
layout(constant_id = 63) const bool YFLIP               = false;

#include "include/bindless.glslinc"

layout(push_constant) uniform CopyImageColorPC {
  uint color_tex;
} copy_pc;

layout(location = 0) in vec2 texCoord0;
layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 uv = texCoord0;
  if (YFLIP) {
    uv.y = 1.0 - uv.y;
  }

  vec4 fragColor = texture(atlas_bindlessSampler2DLinear(copy_pc.color_tex), uv);
  if (DISCARD_TRANSPARENT && fragColor.a == 0.0) {
    discard;
  }
  if (MULTIPLY_ALPHA) {
    fragColor.rgb *= fragColor.a;
  }
  if (DIVIDE_BY_ALPHA && fragColor.a > 0.0) {
    fragColor.rgb /= fragColor.a;
  }

  FragData0 = fragColor;
}
