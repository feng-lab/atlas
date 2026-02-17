#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform DDPBlendPC {
  uint temp_tex;
} ddppc;

layout(location = 0) out vec4 FragData0;

void main()
{
  FragData0 = texelFetch(atlas_bindlessSampler2DNearest(ddppc.temp_tex), ivec2(gl_FragCoord.xy), 0);
  if (FragData0.a == 0.0) discard; // for occlusion query
}
