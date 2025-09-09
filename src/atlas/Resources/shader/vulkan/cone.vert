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

void main()
{
  float bradius = attr_origin.w * 1.0; // size_scale is in transforms/pos already
  float tradius = attr_axis.w * 1.0;

  v_color1 = attr_color1;
  v_color2 = attr_color2;

  vec3 flags = mod(floor(vec3(attr_flags / 256.0, attr_flags / 16.0, attr_flags)), 16.0);
  float rightFlag   = flags.x;                         // 0 or 1
  float upFlag      = flags.y;                         // 0 or 1
  float forwardFlag = flags.z;                         // 0 or 1

  vec3 scaledAxis   = (xf.pos_transform * vec4(attr_axis.xyz, 1.0)).xyz;
  vec3 scaledOrigin = (xf.pos_transform * vec4(attr_origin.xyz, 1.0)).xyz;

  float height = length(scaledAxis);
  float inv_sqr_height = max(height * height, 1e-6);
  inv_sqr_height = 1.0 / inv_sqr_height;

  vec3 h = normalize(scaledAxis);
  v_axis = normalize(xf.pos_transform_normal_matrix * h);

  vec3 u = cross(h, vec3(1.0, 0.0, 0.0));
  if (dot(u, u) < 0.001) u = cross(h, vec3(0.0, 1.0, 0.0));
  u = normalize(u);
  vec3 v = normalize(cross(u, h));

  v_U = normalize(xf.pos_transform_normal_matrix * u);
  v_V = normalize(xf.pos_transform_normal_matrix * v);

  // Compute bounding box vertex
  vec4 vertex = vec4(scaledOrigin, 1.0);
  float sRight   = (2.0 * rightFlag   - 1.0);
  float sForward = (2.0 * forwardFlag - 1.0);

  float rMix = mix(bradius, tradius, upFlag);
  vertex.xyz += upFlag * scaledAxis.xyz;
  vertex.xyz += sRight   * rMix * u;
  vertex.xyz += sForward * rMix * v;

  vec4 base4 = xf.view_matrix * vec4(scaledOrigin, 1.0);
  v_base = base4.xyz;
  vec4 top4  = xf.view_matrix * vec4(scaledOrigin + scaledAxis, 1.0);
  v_top  = top4.xyz;

  v_combo1 = vec4(bradius, tradius, height, inv_sqr_height);

  vec4 eyeSpacePos = xf.view_matrix * vertex;
  v_point = eyeSpacePos.xyz;
  gl_Position = xf.projection_view_matrix * vertex;
}
