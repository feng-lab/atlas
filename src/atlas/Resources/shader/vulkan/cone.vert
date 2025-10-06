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

layout(constant_id = 90) const int CAPS_MODE = 1;

#include "include/matrices_material.glslinc"

void main()
{
  float size_scale = xf.parameters.x;
  float bradius = attr_origin.w * size_scale;
  float tradius = attr_axis.w * size_scale;

  v_color1 = attr_color1;
  v_color2 = attr_color2;

  vec3 flags = mod(floor(vec3(attr_flags / 256.0, attr_flags / 16.0, attr_flags)), 16.0);
  float rightFlag = flags.x; // 0 or 1
  float upFlag = flags.y;    // 0 or 1

  vec3 origin = (xf.pos_transform * vec4(attr_origin.xyz, 1.0)).xyz;
  vec3 topPos = (xf.pos_transform * vec4(attr_origin.xyz + attr_axis.xyz, 1.0)).xyz;
  vec3 axis = topPos - origin;

  float height = length(axis);
  float safeHeight = max(height, 1e-6);
  float inv_sqr_height = 1.0 / (safeHeight * safeHeight);

  vec3 h = (height > 1e-6) ? (axis / height) : vec3(0.0, 0.0, 1.0);
  mat3 view_normal_matrix = compute_view_normal_matrix();
  v_axis = normalize(view_normal_matrix * h);

  vec3 u = cross(h, vec3(1.0, 0.0, 0.0));
  if (dot(u, u) < 0.001) {
    u = cross(h, vec3(0.0, 1.0, 0.0));
  }
  u = normalize(u);
  vec3 v = normalize(cross(u, h));

  v_U = normalize(view_normal_matrix * u);
  v_V = normalize(view_normal_matrix * v);

  vec4 base4 = xf.view_matrix * vec4(origin, 1.0);
  v_base = base4.xyz;
  vec4 top4 = xf.view_matrix * vec4(topPos, 1.0);
  v_top = top4.xyz;

  v_combo1 = vec4(bradius, tradius, height, inv_sqr_height);

  vec4 points[8];
  if (CAPS_MODE == 0 || CAPS_MODE == 1) {
    points[0] = vec4(origin + bradius * (u + v), 1.0);
    points[1] = vec4(origin + bradius * (-u + v), 1.0);
    points[2] = vec4(origin + bradius * (-u - v), 1.0);
    points[3] = vec4(origin + bradius * (u - v), 1.0);
    points[4] = vec4(origin + axis + tradius * (u + v), 1.0);
    points[5] = vec4(origin + axis + tradius * (-u + v), 1.0);
    points[6] = vec4(origin + axis + tradius * (-u - v), 1.0);
    points[7] = vec4(origin + axis + tradius * (u - v), 1.0);
  } else {
    float adjustedTopRadius = tradius + tradius * (tradius - bradius) / safeHeight;
    points[0] = vec4(origin + bradius * (u + v - h), 1.0);
    points[1] = vec4(origin + bradius * (-u + v - h), 1.0);
    points[2] = vec4(origin + bradius * (-u - v - h), 1.0);
    points[3] = vec4(origin + bradius * (u - v - h), 1.0);
    points[4] = vec4(origin + axis + adjustedTopRadius * (u + v) + tradius * h, 1.0);
    points[5] = vec4(origin + axis + adjustedTopRadius * (-u + v) + tradius * h, 1.0);
    points[6] = vec4(origin + axis + adjustedTopRadius * (-u - v) + tradius * h, 1.0);
    points[7] = vec4(origin + axis + adjustedTopRadius * (u - v) + tradius * h, 1.0);
  }

  vec4 clipMin = vec4(0.0);
  vec4 clipMax = vec4(0.0);
  for (int i = 0; i < 8; ++i) {
    vec4 clip = xf.projection_view_matrix * points[i];
    clip.xyz /= clip.w;
    if (i == 0) {
      clipMin = clip;
      clipMax = clip;
    } else {
      clipMin = min(clipMin, clip);
      clipMax = max(clipMax, clip);
    }
  }

  float depth = (clipMin.z < -1.0 && clipMax.z > -1.0) ? -0.999 : clipMin.z;
  if (clipMin.x < -1.0 && clipMax.x > 1.0 && clipMin.y < -1.0 && clipMax.y > 1.0) {
    depth = -2.0;
  }

  vec4 vertex = vec4(mix(clipMin.x, clipMax.x, rightFlag),
                     mix(clipMin.y, clipMax.y, upFlag),
                     depth,
                     1.0);

  vec4 viewPoint = xf.inverse_projection_matrix * vertex;
  v_point = viewPoint.xyz / viewPoint.w;

  gl_Position = vertex;
}
