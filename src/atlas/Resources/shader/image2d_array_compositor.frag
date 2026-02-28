uniform sampler2DArray color_texture;
uniform sampler2DArray depth_texture;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

#define epsilon 1e-6

void main()
{
#if NUM_VOLUMES < 2
#if GLSL_VERSION < 130
  vec4 color = texelFetch2DArray(color_texture, ivec3(gl_FragCoord.xy, 0), 0);
  if (color.a <= 0) {
    discard;
  }
  gl_FragDepth = texelFetch2DArray(depth_texture, ivec3(gl_FragCoord.xy, 0), 0).r;
#else
  vec4 color = texelFetch(color_texture, ivec3(gl_FragCoord.xy, 0), 0);
  if (color.a <= 0) {
    discard;
  }
  gl_FragDepth = texelFetch(depth_texture, ivec3(gl_FragCoord.xy, 0), 0).r;
#endif
  FragData0 = color;
#elif defined(MAX_PROJ_MERGE)
  vec4 color = vec4(0, 0, 0, 0);
  // Use nearest depth (min). RESULT_OPAQUE no-hit pixels are encoded as exit depth
  // in the raycaster shaders, so silhouette fill does not override real hits.
  float depth = 1;
#if GLSL_VERSION < 130
  for (int i = 0; i < NUM_VOLUMES; ++i) {
    vec4 tmpColor = texelFetch2DArray(color_texture, ivec3(gl_FragCoord.xy, i), 0);
    if (tmpColor.a <= 0) {
      continue;
    }
    color = max(color, tmpColor);
    depth = min(depth, texelFetch2DArray(depth_texture, ivec3(gl_FragCoord.xy, i), 0).r);
#else
  for (int i = 0; i < NUM_VOLUMES; ++i) {
    vec4 tmpColor = texelFetch(color_texture, ivec3(gl_FragCoord.xy, i), 0);
    if (tmpColor.a <= 0) {
      continue;
    }
    color = max(color, tmpColor);
    depth = min(depth, texelFetch(depth_texture, ivec3(gl_FragCoord.xy, i), 0).r);
#endif
  }
  if (color.a <= 0) {
    discard;
  }
  FragData0 = color;
  gl_FragDepth = depth;
#else
  vec4 colors[NUM_VOLUMES];
  float depths[NUM_VOLUMES];
  vec4 color;
  float depth;

  int numValidVolumes = 0;
  for (int i = 0; i < NUM_VOLUMES; ++i) {
#if GLSL_VERSION < 130
    colors[numValidVolumes] = texelFetch2DArray(color_texture, ivec3(gl_FragCoord.xy, i), 0);
    if (colors[numValidVolumes].a > 0) {
      depths[numValidVolumes++] = texelFetch2DArray(depth_texture, ivec3(gl_FragCoord.xy, i), 0).r;
    }
#else
    colors[numValidVolumes] = texelFetch(color_texture, ivec3(gl_FragCoord.xy, i), 0);
    if (colors[numValidVolumes].a > 0) {
      depths[numValidVolumes++] = texelFetch(depth_texture, ivec3(gl_FragCoord.xy, i), 0).r;
    }
#endif
  }

  if (numValidVolumes == 0) {
    discard;
  } else if (numValidVolumes == 1) {
    FragData0 = colors[0];
    gl_FragDepth = depths[0];
    return;
  }

  for (int j = 1; j < numValidVolumes; ++j) {
    color = colors[j];
    depth = depths[j];
    int i = j-1;
    while (i >= 0 && depth > depths[i]) {
      depths[i+1] = depths[i];
      colors[i+1] = colors[i];
      --i;
    }
    depths[i+1] = depth;
    colors[i+1] = color;
  }

  if (abs(depths[1] - depths[0]) < epsilon) {
    color = max(colors[1], colors[0]);
  } else {
    color = colors[1] + (1 - colors[1].a) * colors[0];
  }
  for (int i = 2; i < numValidVolumes; ++i) {
    if (abs(depths[i] - depths[i-1]) < epsilon) {
      color = max(color, colors[i]);
    } else {
      color = colors[i] + (1 - colors[i].a) * color;
    }
  }
  FragData0 = color;
  gl_FragDepth = depths[numValidVolumes-1];
#endif
}
