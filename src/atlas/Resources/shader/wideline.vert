#if GLSL_VERSION >= 130
in vec3 attr_vertex;
#ifndef USE_1DTEXTURE
in vec4 attr_color;
#endif
#else
attribute vec3 attr_vertex;
#ifndef USE_1DTEXTURE
attribute vec4 attr_color;
#endif
#endif

uniform mat4 projection_view_matrix;
uniform mat4 pos_transform = mat4(1.0);

#if GLSL_VERSION >= 130 && defined(HAS_CLIP_PLANE)
uniform vec4 clip_planes[CLIP_PLANE_COUNT];
#if CLIP_DISTANCE_COUNT > 0
out float gl_ClipDistance[CLIP_DISTANCE_COUNT];
#endif
#if EXTRA_CLIP_PLANE_COUNT > 0
out vec4 atlas_world_vertex;
#endif
#endif

#ifndef USE_1DTEXTURE
#if GLSL_VERSION >= 130
out vec4 colorIn;
#else
varying vec4 colorIn;
#endif
#endif

void main()
{
  vec4 vertex = pos_transform * vec4(attr_vertex, 1.0);
  gl_Position = projection_view_matrix * vertex;
#ifndef USE_1DTEXTURE
  colorIn = attr_color;
#endif
#if defined(HAS_CLIP_PLANE)
#if GLSL_VERSION >= 130
#if CLIP_DISTANCE_COUNT > 0
  for (int i=0; i<CLIP_DISTANCE_COUNT; ++i)
    gl_ClipDistance[i] = dot(clip_planes[i], vertex);
#endif
#if EXTRA_CLIP_PLANE_COUNT > 0
  atlas_world_vertex = vertex;
#endif
#else
  gl_ClipVertex = vertex;
#endif   // version 130 or up
#endif  // has clipplane
}
