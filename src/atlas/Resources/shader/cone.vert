// note: this shader requires that base radius is smaller than top radius

#if GLSL_VERSION >= 130
in vec4 attr_origin;    // base location + base radius
in vec4 attr_axis;      // axis (= top - base) + top radius
in float attr_flags;
in vec4 attr_color;
in vec4 attr_color2;
#else
attribute vec4 attr_origin;    // base location + base radius
attribute vec4 attr_axis;      // axis (= top - base) + top radius
attribute float attr_flags;
attribute vec4 attr_color;
attribute vec4 attr_color2;
#endif

uniform float size_scale = 1.0;
//uniform vec3 pos_scale = vec3(1.0, 1.0, 1.0);
uniform mat4 pos_transform = mat4(1.0);
uniform mat4 view_matrix;
uniform mat4 projection_view_matrix;
uniform mat3 normal_matrix;

#if GLSL_VERSION >= 130 && defined(HAS_CLIP_PLANE)
uniform vec4 clip_planes[CLIP_PLANE_COUNT];
out float gl_ClipDistance[CLIP_PLANE_COUNT];
#endif

#if GLSL_VERSION >= 130
out vec3 point;
out vec3 axis;
out vec3 base;
out vec3 top;
out vec3 U;
out vec3 V;
out vec4 combo1;
#define bradius combo1.x
#define tradius combo1.y
#define height combo1.z
#define inv_sqr_height combo1.w
//out float bradius;
//out float tradius;
//out float height;
//out float inv_sqr_height;
out vec4 color1;
out vec4 color2;
#else
varying vec3 point;
varying vec3 axis;
varying vec3 base;
varying vec3 top;
varying vec3 U;
varying vec3 V;
varying vec4 combo1;
#define bradius combo1.x
#define tradius combo1.y
#define height combo1.z
#define inv_sqr_height combo1.w
//varying float bradius;
//varying float tradius;
//varying float height;
//varying float inv_sqr_height;
varying vec4 color1;
varying vec4 color2;
#endif

void main(void)
{
  bradius = size_scale * attr_origin.w;
  tradius = size_scale * attr_axis.w;

  color1 = attr_color;
  color2 = attr_color2;

  vec3 flags = mod(floor(vec3(attr_flags/256.0, attr_flags/16.0, attr_flags)), 16.0);
  // either 0 or 1, 0 -> left, 1 -> right
  float rightFlag = flags.x;
  // either 0 or 1, 0 -> down(base), 1 -> up(top)
  float upFlag = flags.y;
  // either 0 ro 1, 0 -> backward, 1 -> forward
  float forwardFlag = flags.z;

  //vec3 scaledAxis = attr_axis.xyz * pos_scale;
  //vec3 scaledOrigin = attr_origin.xyz * pos_scale;
  vec3 scaledAxis = (pos_transform * vec4(attr_axis.xyz, 1.0)).xyz;
  vec3 scaledOrigin = (pos_transform * vec4(attr_origin.xyz, 1.0)).xyz;

  height = length(scaledAxis);
  inv_sqr_height = height * height;
  inv_sqr_height = 1.0 / inv_sqr_height;

  // h is a normalized cylinder axis
  vec3 h = normalize(scaledAxis);

  // axis is the cylinder axis in modelview coordinates
  axis =  normalize(normal_matrix * h);

  // u, v, h is local system of coordinates
  vec3 u = cross(h, vec3(1.0, 0.0, 0.0));
  if (dot(u,u) < 0.001)
    u = cross(h, vec3(0.0, 1.0, 0.0));
  u = normalize(u);
  vec3 v = normalize(cross(u, h));

  // transform to modelview coordinates
  U = normalize(normal_matrix * u);
  V = normalize(normal_matrix * v);

  // compute bounding box vertex position
  vec4 vertex = vec4(scaledOrigin, 1.0);

#if defined(FLAT_CAPS) || defined(NO_CAPS)
  vertex.xyz += upFlag * scaledAxis.xyz;
  vertex.xyz += (2.0 * rightFlag - 1.0) * mix(bradius, tradius, upFlag) * u;
  vertex.xyz += (2.0 * forwardFlag - 1.0) * mix(bradius, tradius, upFlag) * v;
#else
  float adjustedTopRadius = tradius + tradius * (tradius - bradius) / height;

  vertex.xyz += upFlag * scaledAxis.xyz;
  vertex.xyz += (2.0 * rightFlag - 1.0) * mix(bradius, adjustedTopRadius, upFlag) * u;
  vertex.xyz += (2.0 * forwardFlag - 1.0) * mix(bradius, adjustedTopRadius, upFlag) * v;
  vertex.xyz += (2.0 * upFlag - 1.0) * mix(bradius, tradius, upFlag) * h;   // for round cap
#endif

  vec4 base4 = view_matrix * vec4(scaledOrigin, 1.0);
  base = base4.xyz;

  vec4 top4 = view_matrix * vec4(scaledOrigin + scaledAxis, 1.0);
  top = top4.xyz;

  vec4 tvertex = view_matrix * vertex;
  point = tvertex.xyz;

  gl_Position = projection_view_matrix * vertex;
#if defined(HAS_CLIP_PLANE)
#if GLSL_VERSION >= 130
  for (int i=0; i<CLIP_PLANE_COUNT; ++i)
    gl_ClipDistance[i] = dot(clip_planes[i], vertex);
#else
  gl_ClipVertex = vertex;
#endif   // version 130 or up
#endif  // has clipplane
}

