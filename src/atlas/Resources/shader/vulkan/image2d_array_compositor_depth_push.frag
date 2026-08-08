#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "include/bindless.glslinc"

layout(push_constant) uniform Image2DArrayCompositorPC {
  uint color_texture;
  uint depth_texture;
} pc;

layout(set = 1, binding = 0) uniform texture2DArray atlas_image2d_array_depth;

vec4 atlasImage2DArrayColorFetch(ivec3 coord)
{
  return texelFetch(atlas_bindlessSampler2DArrayNearest(pc.color_texture), coord, 0);
}

float atlasImage2DArrayDepthFetch(ivec3 coord)
{
  return texelFetch(atlas_image2d_array_depth, coord, 0).r;
}

#include "include/image2d_array_compositor_common.glslinc"
