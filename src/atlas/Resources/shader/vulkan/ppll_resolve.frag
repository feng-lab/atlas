#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/bindless.glslinc"

layout(push_constant) uniform PPLLResolvePC {
  uint opaque_depth_texture;
} pc;

#define ATLAS_PPLL_OPAQUE_DEPTH_SIZE() \
  textureSize(atlas_bindlessSampler2DNearest(pc.opaque_depth_texture), 0)
#define ATLAS_PPLL_OPAQUE_DEPTH_FETCH(coord) \
  texelFetch(atlas_bindlessSampler2DNearest(pc.opaque_depth_texture), coord, 0).r

#include "include/ppll_resolve_common.glslinc"
