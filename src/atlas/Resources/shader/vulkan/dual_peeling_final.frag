#version 450

layout(set = 0, binding = 0) uniform sampler2D DepthTex;
layout(set = 0, binding = 1) uniform sampler2D FrontBlenderTex;
layout(set = 0, binding = 2) uniform sampler2D BackBlenderTex;


layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 frontColor = texelFetch(FrontBlenderTex, p, 0);
  vec4 backColor  = texelFetch(BackBlenderTex, p, 0);
  // `DepthTex` comes from the DDP init pass where we stored -minDepth (negated so MAX
  // blending keeps the nearest layer). Sampling and negating it here therefore
  // recovers the normalized Vulkan depth directly; no additional ze->zw conversion
  // is required.
  gl_FragDepth = -texelFetch(DepthTex, p, 0).x;

  // premultiplied alpha combine: front + (1 - front.a) * back
  FragData0 = frontColor + (1.0 - frontColor.a) * backColor;
}
