#version 450

// Orientation: 0=X, 1=Y
layout(constant_id = 100) const int ORIENTATION = 0;

layout(set = 0, binding = 0) uniform sampler2D color_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;

layout(push_constant) uniform BlurPC {
  vec2  screen_dim_RCP;
  int   blur_radius;
  float blur_scale;
  float blur_strength;
  float _pad0;
} pc;

layout(location = 0) out vec4 FragData0;

float Gaussian(float x, float deviation)
{
  return (1.0 / sqrt(2.0 * 3.141592 * deviation)) * exp(-((x * x) / (2.0 * deviation)));
}

void main()
{
  int blurAmount = pc.blur_radius * 2;
  vec4 color = vec4(0.0);
  float depth = 1.0;
  float deviation = float(pc.blur_radius) * 0.35;
  deviation *= deviation;
  float strength = 1.0 - pc.blur_strength;

  for (int i = 0; i < 20; ++i) {
    if (i >= blurAmount) break;
    float offset = float(i - pc.blur_radius);
    vec2 o = (ORIENTATION == 0) ? vec2(offset * pc.blur_scale, 0.0)
                                : vec2(0.0, offset * pc.blur_scale);
    float w = Gaussian(offset * strength, deviation);
    vec2 tc = (gl_FragCoord.xy + o) * pc.screen_dim_RCP;
    color += texture(color_texture, tc) * w;
    depth = min(depth, texture(depth_texture, tc).r * w);
  }

  FragData0 = clamp(color, 0.0, 1.0);
  gl_FragDepth = depth;
}

