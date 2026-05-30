#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform CopyRaycasterPC {
  uint color_texture;
  uint depth_texture;
} pc;

layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 pix = ivec2(gl_FragCoord.xy);
  vec2 rayLengthAndDepth = texelFetch(atlas_bindlessSampler2DNearest(pc.depth_texture), pix, 0).xy;
  if (rayLengthAndDepth.x < 1.0) discard;

  vec4 fragColor = texelFetch(atlas_bindlessSampler2DNearest(pc.color_texture), pix, 0);
  FragData0 = fragColor;
  gl_FragDepth = rayLengthAndDepth.y;
}
