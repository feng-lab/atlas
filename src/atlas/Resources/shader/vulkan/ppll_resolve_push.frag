#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

layout(location = 0) out vec4 FragData0;

layout(set = 1, binding = 0) uniform texture2D ppll_opaque_depth;

#define ATLAS_PPLL_OPAQUE_DEPTH_SIZE() textureSize(ppll_opaque_depth, 0)
#define ATLAS_PPLL_OPAQUE_DEPTH_FETCH(coord) texelFetch(ppll_opaque_depth, coord, 0).r

#include "include/ppll_resolve_common.glslinc"
