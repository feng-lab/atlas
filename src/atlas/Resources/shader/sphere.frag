uniform float ortho;

uniform vec4 scene_ambient;
uniform vec4 material_ambient;
uniform vec4 material_specular;
uniform float material_shininess;
uniform float alpha;
uniform mat4 projection_matrix;

#if GLSL_VERSION >= 130
in vec4 color;
in vec3 sphere_center;
in float radius2;
in vec3 point;
#ifdef DYNAMIC_MATERIAL_PROPERTY
in float va_material_shininess;
in vec4 va_material_specular;
#endif
#else
varying vec4 color;
varying vec3 sphere_center;
varying float radius2;
varying vec3 point;
#ifdef DYNAMIC_MATERIAL_PROPERTY
varying float va_material_shininess;
varying vec4 va_material_specular;
#endif
#endif

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

vec4 apply_lighting_and_fog(const in vec4 sceneAmbient,
                            const in float materialShininess, const in vec4 materialAmbient, const in vec4 materialSpecular,
                            const in vec3 normalDirection, const in vec3 position, const in vec4 color, const in float alpha);

void main(void)
{
  vec3 rayOrigin = mix(vec3(0.0 ,0.0, 0.0), point, ortho);
  vec3 rayDirection = mix(normalize(point), vec3(0.0, 0.0, -1.0), ortho);

  vec3 sphereVector = sphere_center - rayOrigin;

  // Calculate sphere-ray intersection
  float b = dot(sphereVector, rayDirection);

  float position = b * b + radius2 - dot(sphereVector, sphereVector);

  // Check if the ray missed the sphere
  if (position < 0.0)
    discard;

  float dist = b - sqrt(position);

  // point of intersection on sphere surface
  vec3 ipoint = dist * rayDirection + rayOrigin;

  // Calculate depth in clipping space
  vec2 clipZW = ipoint.z * projection_matrix[2].zw + projection_matrix[3].zw;

  float depth = 0.5 + 0.5 * clipZW.x / clipZW.y;

  if (depth <= 0.0)
    discard;

  if (depth >= 1.0)
    discard;

  gl_FragDepth = depth;

  vec3 normalDirection = normalize(ipoint - sphere_center);

#ifdef DYNAMIC_MATERIAL_PROPERTY
  FragData0 = apply_lighting_and_fog(scene_ambient, va_material_shininess, material_ambient, va_material_specular,
                                     normalDirection, ipoint, color, alpha);
#else
  FragData0 = apply_lighting_and_fog(scene_ambient, material_shininess, material_ambient, material_specular,
                                     normalDirection, ipoint, color, alpha);
#endif

}
