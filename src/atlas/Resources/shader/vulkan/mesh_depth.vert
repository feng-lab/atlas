#version 450

// Depth-only mesh vertex shader for OIT init/count passes.
//
// Purpose:
// - Avoid exporting varyings that are unused by depth-only fragment shaders
//   (glslc -O optimizes those fragment shaders to have no stage inputs).
// - Eliminates Vulkan validation warnings about vertex outputs not consumed by
//   the fragment stage and reduces vertex shader work for those passes.

layout(location = 0) in vec3 attr_vertex;

#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"

void main()
{
  vec4 vertex = xf.pos_transform * vec4(attr_vertex, 1.0);
  atlas_write_clip_distances(vertex);
  gl_Position = xf.projection_view_matrix * vertex;
}

