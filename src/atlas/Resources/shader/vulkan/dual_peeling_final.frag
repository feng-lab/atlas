#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform DDPFinalPC {
  uint depth_tex;
  uint front_blender_tex;
  uint back_blender_tex;
} ddppc;

layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 frontColor = texelFetch(atlas_bindlessSampler2DNearest(ddppc.front_blender_tex), p, 0);
  vec4 backColor  = texelFetch(atlas_bindlessSampler2DNearest(ddppc.back_blender_tex), p, 0);
  // `DepthTex` comes from the DDP init pass where we stored -minDepth (negated so MAX
  // blending keeps the nearest layer). Sampling and negating it here therefore
  // recovers the normalized Vulkan depth directly; no additional ze->zw conversion
  // is required.
  gl_FragDepth = -texelFetch(atlas_bindlessSampler2DNearest(ddppc.depth_tex), p, 0).x;

  // premultiplied alpha combine: front + (1 - front.a) * back
  FragData0 = frontColor + (1.0 - frontColor.a) * backColor;
}
