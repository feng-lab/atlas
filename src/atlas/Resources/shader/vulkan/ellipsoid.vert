#version 450
#extension GL_GOOGLE_include_directive : require

// Attributes
layout(location = 0) in mat4 attr_T; // variance matrix (T)
layout(location = 4) in vec4 attr_color;
layout(location = 5) in float attr_flags;
layout(location = 6) in vec4 attr_specular_shininess;

// Varyings
layout(location = 0) out vec4 v_color;
layout(location = 1) out mat4 v_MT_inverse;
layout(location = 5) out vec3 v_point;
layout(location = 6) out vec4 v_material_specular;
layout(location = 7) out float v_material_shininess;

#include "include/matrices_material.glslinc"

layout(constant_id = 60) const bool USE_DYNAMIC_MATERIAL = false;

#if 1
mat4 inverse_glsl(const in mat4 m)
{
  float Coef00 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
  float Coef02 = m[1][2] * m[3][3] - m[3][2] * m[1][3];
  float Coef03 = m[1][2] * m[2][3] - m[2][2] * m[1][3];
  float Coef04 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
  float Coef06 = m[1][1] * m[3][3] - m[3][1] * m[1][3];
  float Coef07 = m[1][1] * m[2][3] - m[2][1] * m[1][3];
  float Coef08 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
  float Coef10 = m[1][1] * m[3][2] - m[3][1] * m[1][2];
  float Coef11 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
  float Coef12 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
  float Coef14 = m[1][0] * m[3][3] - m[3][0] * m[1][3];
  float Coef15 = m[1][0] * m[2][3] - m[2][0] * m[1][3];
  float Coef16 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
  float Coef18 = m[1][0] * m[3][2] - m[3][0] * m[1][2];
  float Coef19 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
  float Coef20 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
  float Coef22 = m[1][0] * m[3][1] - m[3][0] * m[1][1];
  float Coef23 = m[1][0] * m[2][1] - m[2][0] * m[1][1];
  vec4 SignA = vec4(1.0, -1.0, 1.0, -1.0);
  vec4 SignB = vec4(-1.0, 1.0, -1.0, 1.0);
  vec4 Fac0 = vec4(Coef00, Coef00, Coef02, Coef03);
  vec4 Fac1 = vec4(Coef04, Coef04, Coef06, Coef07);
  vec4 Fac2 = vec4(Coef08, Coef08, Coef10, Coef11);
  vec4 Fac3 = vec4(Coef12, Coef12, Coef14, Coef15);
  vec4 Fac4 = vec4(Coef16, Coef16, Coef18, Coef19);
  vec4 Fac5 = vec4(Coef20, Coef20, Coef22, Coef23);
  vec4 Vec0 = vec4(m[1][0], m[0][0], m[0][0], m[0][0]);
  vec4 Vec1 = vec4(m[1][1], m[0][1], m[0][1], m[0][1]);
  vec4 Vec2 = vec4(m[1][2], m[0][2], m[0][2], m[0][2]);
  vec4 Vec3 = vec4(m[1][3], m[0][3], m[0][3], m[0][3]);
  vec4 Inv0 = SignA * (Vec1 * Fac0 - Vec2 * Fac1 + Vec3 * Fac2);
  vec4 Inv1 = SignB * (Vec0 * Fac0 - Vec2 * Fac3 + Vec3 * Fac4);
  vec4 Inv2 = SignA * (Vec0 * Fac1 - Vec1 * Fac3 + Vec3 * Fac5);
  vec4 Inv3 = SignB * (Vec0 * Fac2 - Vec1 * Fac4 + Vec2 * Fac5);
  mat4 Inverse = mat4(Inv0, Inv1, Inv2, Inv3);
  vec4 Row0 = vec4(Inverse[0][0], Inverse[1][0], Inverse[2][0], Inverse[3][0]);
  float Determinant = dot(m[0], Row0);
  Inverse = Inverse / Determinant;
  return Inverse;
}
#endif

void main()
{
  // Scale and transform T
  mat4 T;
  T[0] = attr_T[0] * xf.parameters.x;
  T[1] = attr_T[1] * xf.parameters.x;
  T[2] = attr_T[2] * xf.parameters.x;
  T[3] = xf.pos_transform * attr_T[3];

  // Determine bounding quad in clip space
  vec4 D = vec4(1.0, 1.0, 1.0, -1.0);
  mat4 PMT_T = transpose(xf.projection_view_matrix * T);
  float a2 = dot(PMT_T[3] * D, PMT_T[3]) * 2.0;
  float mb = dot(PMT_T[0] * D, PMT_T[3]) * 2.0;
  float c = dot(PMT_T[0] * D, PMT_T[0]);
  float x = (mb + (gl_VertexIndex % 2 == 0 ? 1.0 : -1.0) * sqrt(max(mb*mb - 2.0*a2*c, 0.0))) / a2;
  mb = dot(PMT_T[1] * D, PMT_T[3]) * 2.0;
  c = dot(PMT_T[1] * D, PMT_T[1]);
  float y = (mb + ((gl_VertexIndex / 2) % 2 == 0 ? 1.0 : -1.0) * sqrt(max(mb*mb - 2.0*a2*c, 0.0))) / a2;

  v_MT_inverse = inverse_glsl(xf.view_matrix * T);
  v_color = attr_color;
  if (USE_DYNAMIC_MATERIAL) {
    v_material_specular = vec4(attr_specular_shininess.xyz, 1.0);
    v_material_shininess = attr_specular_shininess.w;
  } else {
    v_material_specular = vec4(0.0);
    v_material_shininess = 0.0;
  }

  vec4 vertex_clipspace = vec4(x, y, 0.0, 1.0);
  vec4 eyeSpacePos = xf.inverse_projection_matrix * vertex_clipspace;
  v_point = eyeSpacePos.xyz / eyeSpacePos.w;
  gl_Position = vertex_clipspace;
}
