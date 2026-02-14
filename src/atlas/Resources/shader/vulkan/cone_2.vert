#version 450
#extension GL_GOOGLE_include_directive : require

// Attributes (match cone.vert)
layout(location = 0) in vec4 attr_origin; // base.xyz, base radius
layout(location = 1) in vec4 attr_axis;   // axis.xyz, top radius
layout(location = 2) in float attr_flags;
layout(location = 3) in vec4 attr_color1;
layout(location = 4) in vec4 attr_color2;

// Varyings (match cone_func.glslinc expectations)
layout(location = 0) out vec4 v_color1;
layout(location = 1) out vec4 v_color2;
layout(location = 2) out vec3 v_point;
layout(location = 3) out vec3 v_axis;
layout(location = 4) out vec3 v_base;
layout(location = 5) out vec3 v_top;
layout(location = 6) out vec3 v_U;
layout(location = 7) out vec3 v_V;
layout(location = 8) out vec4 v_combo1; // bradius, tradius, height, inv_sqr_height

#include "include/matrices_material.glslinc"
#include "include/clip_distance.glslinc"

// Cap specialization (kept in sync with fragment shader)
layout(constant_id = 90) const int CAPS_MODE = 1; // 0=NO_CAPS, 1=FLAT_CAPS, 2=ROUND_CAPS, 3=FLAT_BASE_CAP_ROUND_TOP_CAP, 4=ROUND_BASE_CAP_FLAT_TOP_CAP

void main()
{
  // Match GL path: radii scale with global size scale
  float bradius = attr_origin.w * xo.parameters.x;
  float tradius = attr_axis.w   * xo.parameters.x;

  v_color1 = attr_color1;
  v_color2 = attr_color2;

  // Corner flags: right/up (0 or 1)
  vec2 flags = mod(floor(vec2(attr_flags / 16.0, attr_flags)), 16.0);
  float rightFlag = flags.x;
  float upFlag    = flags.y;

  // Transform endpoints into object space scaled by coord transform
  vec3 scaledOrigin = (xo.pos_transform * vec4(attr_origin.xyz, 1.0)).xyz;
  vec3 scaledTopPos = (xo.pos_transform * vec4(attr_origin.xyz + attr_axis.xyz, 1.0)).xyz;
  vec3 scaledAxis   = scaledTopPos - scaledOrigin;
  atlas_write_clip_distances(vec4(scaledOrigin + scaledAxis * 0.5, 1.0));

  float height = length(scaledAxis);
  float inv_sqr_height = 1.0 / max(height * height, 1e-12);

  // Local frame (u, v, h)
  vec3 h = normalize(scaledAxis);
  mat3 view_normal_matrix = compute_view_normal_matrix();
  v_axis = normalize(view_normal_matrix * h);

  vec3 u = cross(h, vec3(1.0, 0.0, 0.0));
  if (dot(u, u) < 0.001) u = cross(h, vec3(0.0, 1.0, 0.0));
  u = normalize(u);
  vec3 v = normalize(cross(u, h));

  v_U = normalize(view_normal_matrix * u);
  v_V = normalize(view_normal_matrix * v);

  // Build 8 bounding points depending on cap mode
  vec4 p1, p2, p3, p4, p5, p6, p7, p8;
  if (CAPS_MODE == 1 || CAPS_MODE == 0) { // FLAT_CAPS or NO_CAPS
    p1 = vec4(scaledOrigin + bradius * (u + v), 1.0);
    p2 = vec4(scaledOrigin + bradius * (-u + v), 1.0);
    p3 = vec4(scaledOrigin + bradius * (-u - v), 1.0);
    p4 = vec4(scaledOrigin + bradius * (u - v), 1.0);
    p5 = vec4(scaledOrigin + scaledAxis + tradius * (u + v), 1.0);
    p6 = vec4(scaledOrigin + scaledAxis + tradius * (-u + v), 1.0);
    p7 = vec4(scaledOrigin + scaledAxis + tradius * (-u - v), 1.0);
    p8 = vec4(scaledOrigin + scaledAxis + tradius * (u - v), 1.0);
  } else { // ROUND_CAPS variants
    float adjustedTopRadius = tradius + tradius * (tradius - bradius) / max(height, 1e-12);
    p1 = vec4(scaledOrigin + bradius * (u + v - h), 1.0);
    p2 = vec4(scaledOrigin + bradius * (-u + v - h), 1.0);
    p3 = vec4(scaledOrigin + bradius * (-u - v - h), 1.0);
    p4 = vec4(scaledOrigin + bradius * (u - v - h), 1.0);
    p5 = vec4(scaledOrigin + scaledAxis + adjustedTopRadius * (u + v) + tradius * h, 1.0);
    p6 = vec4(scaledOrigin + scaledAxis + adjustedTopRadius * (-u + v) + tradius * h, 1.0);
    p7 = vec4(scaledOrigin + scaledAxis + adjustedTopRadius * (-u - v) + tradius * h, 1.0);
    p8 = vec4(scaledOrigin + scaledAxis + adjustedTopRadius * (u - v) + tradius * h, 1.0);
  }

  // Project to clip and divide by w
  p1 = xf.projection_view_matrix * p1;
  p2 = xf.projection_view_matrix * p2;
  p3 = xf.projection_view_matrix * p3;
  p4 = xf.projection_view_matrix * p4;
  p5 = xf.projection_view_matrix * p5;
  p6 = xf.projection_view_matrix * p6;
  p7 = xf.projection_view_matrix * p7;
  p8 = xf.projection_view_matrix * p8;

  p1.xyz /= p1.w; p2.xyz /= p2.w; p3.xyz /= p3.w; p4.xyz /= p4.w;
  p5.xyz /= p5.w; p6.xyz /= p6.w; p7.xyz /= p7.w; p8.xyz /= p8.w;

  vec4 pmin = p1;
  pmin = min(pmin, p2); pmin = min(pmin, p3); pmin = min(pmin, p4);
  pmin = min(pmin, p5); pmin = min(pmin, p6); pmin = min(pmin, p7); pmin = min(pmin, p8);

  vec4 pmax = p1;
  pmax = max(pmax, p2); pmax = max(pmax, p3); pmax = max(pmax, p4);
  pmax = max(pmax, p5); pmax = max(pmax, p6); pmax = max(pmax, p7); pmax = max(pmax, p8);

  // Vulkan clip volume: x/y in [-w,w], z in [0,w]. We emit NDC coordinates
  // with w=1, so allowing z outside [0,1] is important: it lets fixed-function
  // clipping reject cones that are entirely outside the view (e.g., behind the
  // near plane) instead of rasterizing huge quads that later discard in the
  // fragment shader.
  //
  // If the bounding prism crosses the near plane, push slightly forward so the
  // quad stays in front of the clear depth (same intent as the GL path).
  float depth = (pmin.z < 0.0 && pmax.z > 0.0) ? 0.001 : pmin.z;

  // If the projected bounds blow up to (almost) full-screen, match the GL path
  // by pushing the quad outside the clip volume so it is clipped away early.
  if (pmin.x < -1.0 && pmax.x > 1.0 && pmin.y < -1.0 && pmax.y > 1.0) {
    depth = -1.0;
  }

  // Output vertex at selected corner of bounding quad
  gl_Position = vec4(mix(pmin.x, pmax.x, rightFlag),
                     mix(pmin.y, pmax.y, upFlag),
                     depth,
                     1.0);

  // Pass eye-space point by inverting projection
  vec4 eyeSpace = xf.inverse_projection_matrix * gl_Position;
  v_point = eyeSpace.xyz / max(eyeSpace.w, 1e-12);

  // Base/top in eye space
  v_base = (xf.view_matrix * vec4(scaledOrigin, 1.0)).xyz;
  v_top  = (xf.view_matrix * vec4(scaledOrigin + scaledAxis, 1.0)).xyz;

  v_combo1 = vec4(bradius, tradius, height, inv_sqr_height);
}
