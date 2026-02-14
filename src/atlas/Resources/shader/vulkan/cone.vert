#version 450

// Attributes
layout(location = 0) in vec4 attr_origin; // base.xyz, base radius
layout(location = 1) in vec4 attr_axis;   // axis.xyz, top radius
layout(location = 2) in float attr_flags;
layout(location = 3) in vec4 attr_color1;
layout(location = 4) in vec4 attr_color2;

// Varyings
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

// Cap style specialization to mirror GL #defines
layout(constant_id = 90) const int CAPS_MODE = 1; // 0=NO_CAPS, 1=FLAT_CAPS, 2=ROUND_CAPS, 3=FLAT_BASE_CAP_ROUND_TOP_CAP, 4=ROUND_BASE_CAP_FLAT_TOP_CAP

void main()
{
  // Match GL: radii are scaled by global sizeScale (xf.parameters.x).
  float bradius = attr_origin.w * xo.parameters.x;
  float tradius = attr_axis.w * xo.parameters.x;

  v_color1 = attr_color1;
  v_color2 = attr_color2;

  vec3 flags = mod(floor(vec3(attr_flags / 256.0, attr_flags / 16.0, attr_flags)), 16.0);
  float rightFlag   = flags.x;                         // 0 or 1
  float upFlag      = flags.y;                         // 0 or 1
  float forwardFlag = flags.z;                         // 0 or 1

  vec3 scaledAxis   = (xo.pos_transform * vec4(attr_axis.xyz, 1.0)).xyz;
  vec3 scaledOrigin = (xo.pos_transform * vec4(attr_origin.xyz, 1.0)).xyz;

  float height = length(scaledAxis);
  float inv_sqr_height = max(height * height, 1e-6);
  inv_sqr_height = 1.0 / inv_sqr_height;

  vec3 h = normalize(scaledAxis);
  mat3 view_normal_matrix = compute_view_normal_matrix();
  v_axis = normalize(view_normal_matrix * h);

  vec3 u = cross(h, vec3(1.0, 0.0, 0.0));
  if (dot(u, u) < 0.001) u = cross(h, vec3(0.0, 1.0, 0.0));
  u = normalize(u);
  vec3 v = normalize(cross(u, h));

  v_U = normalize(view_normal_matrix * u);
  v_V = normalize(view_normal_matrix * v);

  // Compute bounding box vertex (account for cap styles, as in GL)
  vec4 vertex = vec4(scaledOrigin, 1.0);
  float sRight   = (2.0 * rightFlag   - 1.0);
  float sForward = (2.0 * forwardFlag - 1.0);

  float rMix = mix(bradius, tradius, upFlag);
  vertex.xyz += upFlag * scaledAxis.xyz;
  if (CAPS_MODE == 0 || CAPS_MODE == 1) {
    // NO_CAPS or FLAT_CAPS: linear radius interpolation in u/v plane
    vertex.xyz += sRight   * rMix * u;
    vertex.xyz += sForward * rMix * v;
  } else {
    // ROUND_CAPS variants: expand top cross-section and add axial offset for round cap
    float adjustedTopRadius = tradius + tradius * (tradius - bradius) / max(height, 1e-12);
    float rMixAdj = mix(bradius, adjustedTopRadius, upFlag);
    vertex.xyz += sRight   * rMixAdj * u;
    vertex.xyz += sForward * rMixAdj * v;
    vertex.xyz += (2.0 * upFlag - 1.0) * mix(bradius, tradius, upFlag) * h;
  }

  vec4 base4 = xf.view_matrix * vec4(scaledOrigin, 1.0);
  v_base = base4.xyz;
  vec4 top4  = xf.view_matrix * vec4(scaledOrigin + scaledAxis, 1.0);
  v_top  = top4.xyz;

  v_combo1 = vec4(bradius, tradius, height, inv_sqr_height);

  vec4 eyeSpacePos = xf.view_matrix * vertex;
  v_point = eyeSpacePos.xyz;
  atlas_write_clip_distances(vertex);
  gl_Position = xf.projection_view_matrix * vertex;
}
