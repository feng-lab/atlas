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

void fragment_func(out vec4 fragColor, out float fragDepth)
{
#if defined(HAS_CLIP_PLANE) && GLSL_VERSION >= 130 && EXTRA_CLIP_PLANE_COUNT > 0
  if (clip_planes_enabled) {
    for (int i = 0; i < EXTRA_CLIP_PLANE_COUNT; ++i) {
      if (atlas_extra_clip_distance[i] < 0.0)
        discard;
    }
  }
#endif
  fragDepth = gl_FragCoord.z;
  fragColor = !lighting_enabled ? color : vec4(color.rgb * color.a * alpha, color.a * alpha);
}
