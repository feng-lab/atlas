#version 450
#extension GL_GOOGLE_include_directive : require

// Attributes
layout(location = 0) in vec3 attr_p0;
layout(location = 1) in vec3 attr_p1;
layout(location = 2) in vec4 attr_p0color; // optional
layout(location = 3) in vec4 attr_p1color; // optional
layout(location = 4) in float attr_flags;

// Varyings
layout(location = 0) out vec4 v_color;
layout(location = 1) flat out vec3 plane1;
layout(location = 2) flat out vec3 plane2;
layout(location = 3) flat out vec3 plane3;
layout(location = 4) flat out vec3 plane4;
layout(location = 5) flat out vec4 p0p1;

#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"
#include "include/wideline_common.glslinc"

// Specialization constant for screen-aligned mode (parity with GL macro)
layout(constant_id = 101) const bool LINE_SCREEN_ALIGNED = false;

void ClipSegmentToPlane(inout vec4 p0, inout vec4 p1, vec4 plane)
{
  float dist0 = dot(p0, plane);
  float dist1 = dot(p1, plane);
  bool in0 = dist0 >= 0.0;
  bool in1 = dist1 >= 0.0;
  if (!in0 && !in1) {
    p0 = vec4(0,0,0,-1);
    p1 = vec4(0,0,0,-1);
  } else if (in0 != in1) {
    float t = dist0 / (dist0 - dist1);
    if (in1) p0 = mix(p0, p1, t);
    else     p1 = mix(p0, p1, t);
  }
}

void main()
{
  vec2 flags = mod(floor(vec2(attr_flags/16.0, attr_flags)), 16.0);
  float rightFlag = flags.x - 1.0;
  float upFlag    = flags.y - 1.0;

  // Compute clip-space endpoints first
  vec4 p0vertex = xo.pos_transform * vec4(attr_p0, 1.0);
  vec4 p1vertex = xo.pos_transform * vec4(attr_p1, 1.0);
  vec4 p0clip = xf.projection_view_matrix * p0vertex;
  vec4 p1clip = xf.projection_view_matrix * p1vertex;
  atlas_write_clip_distances(rightFlag > 0.0 ? p1vertex : p0vertex);
  // Clip against the near plane in clip coordinates. Vulkan uses depth in [0,1]
  // so the clip half-space is z >= 0 (i.e., plane (0,0,1,0)).
  ClipSegmentToPlane(p0clip, p1clip, vec4(0,0,1,0));
  // Perspective divide to NDC, then to viewport for 2D expansion
  p0clip /= p0clip.w; p1clip /= p1clip.w;
  vec4 p0 = wpc.viewport_matrix * p0clip;
  vec4 p1 = wpc.viewport_matrix * p1clip;
  // Provide endpoints for round caps; harmless if not used in FS
  p0p1 = vec4(p0.xy, p1.xy);

  float R = (wpc.line_width * wpc.size_scale / 2.0 + 1.0);
  vec2 L = normalize(p1.xy - p0.xy);
  vec2 P = vec2(-L.y, L.x);
  vec2 LR = LINE_SCREEN_ALIGNED ? vec2(0.0) : L * R;
  vec2 PR = P * R;
  if (LINE_SCREEN_ALIGNED) {
    vec2 Lmajor = abs(L.y) >= abs(L.x) ? vec2(0, sign(L.y)) : vec2(sign(L.x), 0);
    PR -= dot(PR, Lmajor) / max(dot(L, Lmajor), 1e-6) * L;
  }

  vec2 qcorner = upFlag * PR + (rightFlag > 0.0 ? (p1.xy + LR) : (p0.xy - LR));

  plane1 = vec3(+P, -(dot(p0.xy, +P) - R));
  plane2 = vec3(-P, -(dot(p1.xy, -P) - R));
  if (LINE_SCREEN_ALIGNED) {
    vec2 Lmajor2 = abs(L.y) >= abs(L.x) ? vec2(0, sign(L.y)) : vec2(sign(L.x), 0);
    plane3 = vec3(+Lmajor2, -(dot(p0.xy, +Lmajor2) - 1.0));
    plane4 = vec3(-Lmajor2, -(dot(p1.xy, -Lmajor2) - 1.0));
  } else {
    plane3 = vec3(+L, -(dot(p0.xy, +L) - 1.0));
    plane4 = vec3(-L, -(dot(p1.xy, -L) - 1.0));
  }

  // interpolate color per-segment end
  v_color = rightFlag > 0.0 ? attr_p1color : attr_p0color;

  // Use viewport-space z,w (mapped from NDC by viewport_matrix) so that
  // inverseViewportMatrix restores correct clip/NDC depth.
  gl_Position = wpc.viewport_matrix_inverse * vec4(qcorner, rightFlag > 0.0 ? p1.zw : p0.zw);
}
