uniform vec2 screen_dim_RCP;

uniform sampler2D color_texture;
uniform sampler2D depth_texture;

uniform int blur_radius;
uniform float blur_scale;
uniform float blur_strength;

#if GLSL_VERSION >= 330
layout(location = 0) out vec4 FragData0;
#elif GLSL_VERSION >= 130
out vec4 FragData0;  // call glBindFragDataLocation before linking
#else
#define FragData0 gl_FragData[0]
#endif

#if GLSL_VERSION < 130
#define texture texture2D
#endif

/// <summary>
/// Gets the Gaussian value in the first dimension.
/// </summary>
/// <param name="x">Distance from origin on the x-axis.</param>
/// <param name="deviation">Standard deviation.</param>
/// <returns>The gaussian value on the x-axis.</returns>
float Gaussian(float x, float deviation)
{
  return (1.0 / sqrt(2.0 * 3.141592 * deviation)) * exp(-((x * x) / (2.0 * deviation)));  
}

void main()
{
  int blurAmount = blur_radius * 2;
  vec4 color = vec4(0.0);
  float depth = 1;
  float deviation = float(blur_radius) * 0.35;
  deviation *= deviation;
  float strength = 1 - blur_strength;

#if defined(ORIENTATION_X)
    for (int i=0; i<20; ++i) {
      if (i >= blurAmount)
        break;

      float offset = i - blur_radius;
      color += texture(color_texture, (gl_FragCoord.xy + vec2(offset * blur_scale, 0.0)) * screen_dim_RCP) * Gaussian(offset * strength, deviation);
      depth = min(depth, texture(depth_texture, (gl_FragCoord.xy + vec2(offset * blur_scale, 0.0)) * screen_dim_RCP).r * Gaussian(offset * strength, deviation));
    }
#elif defined(ORIENTATION_Y)
    for (int i=0; i<20; ++i) {
      if (i >= blurAmount)
        break;

      float offset = i - blur_radius;
      color += texture(color_texture, (gl_FragCoord.xy + vec2(0.0, offset * blur_scale)) * screen_dim_RCP) * Gaussian(offset * strength, deviation);
      depth = min(depth, texture(depth_texture, (gl_FragCoord.xy + vec2(0.0, offset * blur_scale)) * screen_dim_RCP).r * Gaussian(offset * strength, deviation));
    }
#endif

  FragData0 = clamp(color, 0.0, 1.0);
  gl_FragDepth = depth;
}
