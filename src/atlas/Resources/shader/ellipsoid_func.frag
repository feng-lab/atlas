uniform float ortho;

uniform vec4 scene_ambient;
uniform vec4 material_ambient;
uniform vec4 material_specular;
uniform float material_shininess;
uniform float alpha;
uniform mat4 projection_matrix;

#if GLSL_VERSION >= 130
in vec4 color;
in mat4 MT_inverse;
in vec3 point;
#if defined(HAS_CLIP_PLANE) && EXTRA_CLIP_PLANE_COUNT > 0
in float atlas_extra_clip_distance[EXTRA_CLIP_PLANE_COUNT];
#endif
#ifdef DYNAMIC_MATERIAL_PROPERTY
in float va_material_shininess;
in vec4 va_material_specular;
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

vec4 apply_lighting_and_fog(const in vec4 sceneAmbient,
                            const in float materialShininess, const in vec4 materialAmbient, const in vec4 materialSpecular,
                            const in vec3 normalDirection, const in vec3 position, const in vec4 color, const in float alpha);

void fragment_func(out vec4 fragColor, out float fragDepth)
{
#if defined(HAS_CLIP_PLANE) && GLSL_VERSION >= 130 && EXTRA_CLIP_PLANE_COUNT > 0
  if (clip_planes_enabled) {
    for (int i = 0; i < EXTRA_CLIP_PLANE_COUNT; ++i) {
      if (atlas_extra_clip_distance[i] < 0.0)
        discard;
    }
  }
#endif
  vec3 rayOrigin = mix(vec3(0.0 ,0.0, 0.0), point, ortho);
  vec3 rayDirection = mix(normalize(point), vec3(0.0, 0.0, -1.0), ortho);

  vec4 xfpp = MT_inverse * vec4(rayOrigin, 1.0);
  vec4 c3 = MT_inverse * vec4(rayDirection, 0.0);

  vec4 D = vec4(1.0, 1.0, 1.0, -1.0);
  float a = dot(c3 * D, c3);
  float b = 2.0 * dot(xfpp * D, c3);
  float c = dot(xfpp * D, xfpp);
  float dist = b*b - 4*a*c;

  if (dist < 0)
    discard;

  dist = (-b - sqrt(dist)) / (2*a);

  // point of intersection on ellipsoid surface
  vec3 ipoint = dist * rayDirection + rayOrigin;

  // Calculate depth in clipping space
  vec2 clipZW = ipoint.z * projection_matrix[2].zw + projection_matrix[3].zw;

  float depth = 0.5 + 0.5 * clipZW.x / clipZW.y;

  if (depth <= 0.0)
    discard;

  if (depth >= 1.0)
    discard;

  fragDepth = depth;

  vec4 normal4 = transpose(MT_inverse) * (xfpp + dist * c3);
  vec3 normalDirection = normalize(normal4.xyz);

#ifdef DYNAMIC_MATERIAL_PROPERTY
  fragColor = apply_lighting_and_fog(scene_ambient, va_material_shininess, material_ambient, va_material_specular,
                                     normalDirection, ipoint, color, alpha);
#else
  fragColor = apply_lighting_and_fog(scene_ambient, material_shininess, material_ambient, material_specular,
                                     normalDirection, ipoint, color, alpha);
#endif
}
