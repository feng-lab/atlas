#version 450

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

#include "include/wideline_common.glslinc"

layout(location = 0) in vec4 colorIn[];
layout(location = 0) out vec4 color;

// Outputs to fragment to match wideline_func1.glslinc
layout(location = 1) flat out vec3 plane1;
layout(location = 2) flat out vec3 plane2;
layout(location = 3) flat out vec3 plane3;
layout(location = 4) flat out vec3 plane4;
layout(location = 5) flat out vec4 p0p1; // endpoints for round caps (optional)

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
  vec4 p0 = gl_in[0].gl_Position;
  vec4 p1 = gl_in[1].gl_Position;
  ClipSegmentToPlane(p0, p1, vec4(0,0,1,1));

  p0 = wpc.viewport_matrix * p0; p0 /= p0.w;
  p1 = wpc.viewport_matrix * p1; p1 /= p1.w;

  float R = (wpc.line_width * wpc.size_scale / 2.0 + 1.0);
  vec2 L = normalize(p1.xy - p0.xy);
  vec2 P = vec2(-L.y, L.x);
  vec2 LR = L * R;
  vec2 PR = P * R;

  vec2 q0 = p0.xy - LR - PR;
  vec2 q1 = p1.xy + LR - PR;
  vec2 q2 = p1.xy + LR + PR;
  vec2 q3 = p0.xy - LR + PR;

  // Return to clip space
  vec4 v0 = wpc.viewport_matrix_inverse * vec4(q0, p0.zw);
  vec4 v1 = wpc.viewport_matrix_inverse * vec4(q1, p1.zw);
  vec4 v2 = wpc.viewport_matrix_inverse * vec4(q2, p1.zw);
  vec4 v3 = wpc.viewport_matrix_inverse * vec4(q3, p0.zw);

  // Planes
  plane1 = vec3(+P, -(dot(p0.xy, +P) - R));
  plane2 = vec3(-P, -(dot(p1.xy, -P) - R));
  plane3 = vec3(+L, -(dot(p0.xy, +L) - 1.0));
  plane4 = vec3(-L, -(dot(p1.xy, -L) - 1.0));
  p0p1 = vec4(p0.xy, p1.xy);

  // Emit vertices v0, v3, v1, v2
  color = colorIn[0];
  gl_Position = v0; EmitVertex();
  color = colorIn[0];
  gl_Position = v3; EmitVertex();
  color = colorIn[1];
  gl_Position = v1; EmitVertex();
  color = colorIn[1];
  gl_Position = v2; EmitVertex();
  EndPrimitive();
}
