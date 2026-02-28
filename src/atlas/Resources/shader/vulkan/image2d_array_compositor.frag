#version 450
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

layout(push_constant) uniform Image2DArrayCompositorPC {
  uint color_texture;
  uint depth_texture;
} pc;

// Controls
layout(constant_id = 70) const int  NUM_VOLUMES = 1;   // actual layer count
layout(constant_id = 71) const bool MAX_PROJ_MERGE = false;

layout(location = 0) out vec4 FragData0;

// Upper bound for sorting buffers
const int MAX_VOLUMES = 16;

void main()
{
  if (NUM_VOLUMES < 1) { discard; }

  if (NUM_VOLUMES < 2) {
    vec4 color =
      texelFetch(atlas_bindlessSampler2DArrayNearest(pc.color_texture), ivec3(ivec2(gl_FragCoord.xy), 0), 0);
    if (color.a <= 0.0) discard;
    float depth =
      texelFetch(atlas_bindlessSampler2DArrayNearest(pc.depth_texture), ivec3(ivec2(gl_FragCoord.xy), 0), 0).r;
    FragData0 = color;
    gl_FragDepth = depth;
    return;
  }

  if (MAX_PROJ_MERGE) {
    vec4 color = vec4(0.0);
    // Use nearest depth (min). RESULT_OPAQUE no-hit pixels are encoded as exit depth
    // in the raycaster shaders, so silhouette fill does not override real hits.
    float depth = 1.0;
    int n = min(NUM_VOLUMES, MAX_VOLUMES);
    for (int i = 0; i < n; ++i) {
      vec4 tmpColor =
        texelFetch(atlas_bindlessSampler2DArrayNearest(pc.color_texture), ivec3(ivec2(gl_FragCoord.xy), i), 0);
      if (tmpColor.a <= 0.0) continue;
      color = max(color, tmpColor);
      float d = texelFetch(atlas_bindlessSampler2DArrayNearest(pc.depth_texture), ivec3(ivec2(gl_FragCoord.xy), i), 0).r;
      depth = min(depth, d);
    }
    if (color.a <= 0.0) discard;
    FragData0 = color;
    gl_FragDepth = depth;
    return;
  }

  vec4 colors[MAX_VOLUMES];
  float depths[MAX_VOLUMES];
  int numValid = 0;
  int n = min(NUM_VOLUMES, MAX_VOLUMES);

  for (int i = 0; i < n; ++i) {
    vec4 c =
      texelFetch(atlas_bindlessSampler2DArrayNearest(pc.color_texture), ivec3(ivec2(gl_FragCoord.xy), i), 0);
    if (c.a > 0.0) {
      colors[numValid] = c;
      depths[numValid] =
        texelFetch(atlas_bindlessSampler2DArrayNearest(pc.depth_texture), ivec3(ivec2(gl_FragCoord.xy), i), 0).r;
      ++numValid;
    }
  }

  if (numValid == 0) { discard; }
  if (numValid == 1) {
    FragData0 = colors[0];
    gl_FragDepth = depths[0];
    return;
  }

  // Insertion sort by depth descending (far -> near)
  for (int j = 1; j < numValid; ++j) {
    vec4 c = colors[j];
    float d = depths[j];
    int i = j - 1;
    while (i >= 0 && d > depths[i]) {
      depths[i + 1] = depths[i];
      colors[i + 1] = colors[i];
      --i;
    }
    depths[i + 1] = d;
    colors[i + 1] = c;
  }

  vec4 color;
  const float epsilon = 1e-6;
  if (abs(depths[1] - depths[0]) < epsilon) color = max(colors[1], colors[0]);
  else color = colors[1] + (1.0 - colors[1].a) * colors[0];
  for (int i = 2; i < numValid; ++i) {
    if (abs(depths[i] - depths[i - 1]) < epsilon) color = max(color, colors[i]);
    else color = colors[i] + (1.0 - colors[i].a) * color;
  }
  FragData0 = color;
  gl_FragDepth = depths[numValid - 1];
}
