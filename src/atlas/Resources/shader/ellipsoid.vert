#if GLSL_VERSION >= 130
in mat4 attr_T;    // variance matrix, Xo'QXo = 0, Xp'DXp = 0
in vec4 attr_color;
in float attr_flags;
#ifdef DYNAMIC_MATERIAL_PROPERTY
in vec4 attr_specular_shininess;
#endif
#else
attribute mat4 attr_T;    // variance matrix, Xo'QXo = 0, Xp'DXp = 0
attribute vec4 attr_color;
attribute float attr_flags;
#ifdef DYNAMIC_MATERIAL_PROPERTY
attribute vec4 attr_specular_shininess;
#endif
#endif

uniform float size_scale = 1.0;
//uniform vec3 pos_scale = vec3(1.0, 1.0, 1.0);
uniform mat4 pos_transform = mat4(1.0);
uniform mat4 projection_view_matrix;
uniform mat4 projection_matrix_inverse;
uniform mat4 view_matrix;
#if GLSL_VERSION >= 130 && defined(HAS_CLIP_PLANE)
uniform vec4 clip_planes[CLIP_PLANE_COUNT];
#if CLIP_DISTANCE_COUNT > 0
out float gl_ClipDistance[CLIP_DISTANCE_COUNT];
#endif
#if EXTRA_CLIP_PLANE_COUNT > 0
out float atlas_extra_clip_distance[EXTRA_CLIP_PLANE_COUNT];
#endif
#endif

#if GLSL_VERSION >= 130
out vec4 color;
out mat4 MT_inverse;
out vec3 point;
#ifdef DYNAMIC_MATERIAL_PROPERTY
out float va_material_shininess;
out vec4 va_material_specular;
#endif
#else
varying vec4 color;
varying mat4 MT_inverse;
varying vec3 point;
#ifdef DYNAMIC_MATERIAL_PROPERTY
varying float va_material_shininess;
varying vec4 va_material_specular;
#endif
#endif

#if GLSL_VERSION < 140
mat4 inverse(const in mat4 m)
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

void main(void)
{
  vec4 D = vec4(1.0, 1.0, 1.0, -1.0);

  // apply scale
  mat4 T;
  T[0] = attr_T[0] * size_scale;
  T[1] = attr_T[1] * size_scale;
  T[2] = attr_T[2] * size_scale;
  //T[3] = attr_T[3] * vec4(pos_scale, 1.0);
  T[3] = pos_transform * attr_T[3];

  // get corner pos
  vec2 flags = mod(floor(vec2(attr_flags/16.0, attr_flags)), 16.0);
  // either -1 or 1, -1 -> left, 1 -> right
  float rightFlag = flags.x - 1.;
  // either -1 or 1, -1 -> down, 1 -> up
  float upFlag = flags.y - 1.;

  // get border
  mat4 PMT_T = transpose(projection_view_matrix * T);
  float a2 = dot(PMT_T[3] * D, PMT_T[3]) * 2.0;   // 2*a
  float mb = dot(PMT_T[0] * D, PMT_T[3]) * 2.0;   // -b
  float c = dot(PMT_T[0] * D, PMT_T[0]);
  float x = (mb + rightFlag * sqrt(mb*mb - 2*a2*c)) / a2;

  mb = dot(PMT_T[1] * D, PMT_T[3]) * 2.0;   // -b
  c = dot(PMT_T[1] * D, PMT_T[1]);
  float y = (mb + upFlag * sqrt(mb*mb - 2*a2*c)) / a2;

  // other
  MT_inverse = inverse(view_matrix * T);

  color = attr_color;

#ifdef DYNAMIC_MATERIAL_PROPERTY
  va_material_specular = vec4(attr_specular_shininess.xyz, 1.);
  va_material_shininess = attr_specular_shininess.w;
#endif

  vec4 vertex_clipspace = vec4(x, y, 0.0, 1.0);
    // Calculate vertex position in modelview space
  vec4 eyeSpacePos = projection_matrix_inverse * vertex_clipspace;
  point = eyeSpacePos.xyz / eyeSpacePos.w;

  // Pass transformed vertex
  gl_Position = vertex_clipspace;
#if defined(HAS_CLIP_PLANE)
#if GLSL_VERSION >= 130
#if CLIP_DISTANCE_COUNT > 0
  for (int i=0; i<CLIP_DISTANCE_COUNT; ++i)
    gl_ClipDistance[i] = dot(clip_planes[i], T[3]);
#endif
#if EXTRA_CLIP_PLANE_COUNT > 0
  for (int i=0; i<EXTRA_CLIP_PLANE_COUNT; ++i)
    atlas_extra_clip_distance[i] = dot(clip_planes[CLIP_DISTANCE_COUNT + i], T[3]);
#endif
#else
  gl_ClipVertex = T[3];
#endif   // version 130 or up
#endif  // has clipplane
}
