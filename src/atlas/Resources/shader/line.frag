uniform float alpha;
uniform bool lighting_enabled = true;

#if GLSL_VERSION >= 130
in vec4 color;
#if defined(HAS_CLIP_PLANE) && EXTRA_CLIP_PLANE_COUNT > 0
in float atlas_extra_clip_distance[EXTRA_CLIP_PLANE_COUNT];
#endif
#else
varying vec4 color;
#endif

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

void main(void)
{
#if defined(HAS_CLIP_PLANE) && GLSL_VERSION >= 130 && EXTRA_CLIP_PLANE_COUNT > 0
  if (clip_planes_enabled) {
    for (int i = 0; i < EXTRA_CLIP_PLANE_COUNT; ++i) {
      if (atlas_extra_clip_distance[i] < 0.0)
        discard;
    }
  }
#endif
  FragData0 = !lighting_enabled ? color : vec4(color.rgb * color.a * alpha, color.a * alpha);
}
