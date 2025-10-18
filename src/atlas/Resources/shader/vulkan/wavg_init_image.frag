#version 450
#extension GL_GOOGLE_include_directive : require

// Weighted Average OIT init for image layers
// Inputs: color_texture (RGBA), depth_texture (R)
// Outputs:
//  - FragAccum (location=0, RGBA32F): accumulates (rgb*alpha, alpha)
//  - FragMoments (location=1, RG32F): accumulates (n, depth)

layout(location = 0) out vec4 FragAccum;
layout(location = 1) out vec2 FragMoments;

#include "include/copyimage_func.glslinc"

void main()
{
  vec4 c; float d;
  fragment_func(c, d);

  float a = c.a;
  // Sum RGB premultiplied by alpha in .rgb; store sum of alpha in .a
  FragAccum = vec4(c.rgb * a, a);
  // For images, treat each fragment as one sample (n += 1), and sum depths
  FragMoments = vec2(1.0, d);
  // Write depth so the init pass respects the provided depth attachment. This
  // ensures contributions behind opaque geometry are culled during WA init.
  gl_FragDepth = d;
}
