#version 450

layout(set = 0, binding = 0) uniform sampler2D color_texture;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;

layout(location = 0) out vec4 FragData0;

void main()
{
  ivec2 pix = ivec2(gl_FragCoord.xy);
  vec4 fragColor = texelFetch(color_texture, pix, 0);
  if (fragColor.a == 0.0) discard;

  vec2 rayLengthAndDepth = texelFetch(depth_texture, pix, 0).xy;
  if (rayLengthAndDepth.x < 1.0) discard;

  FragData0 = fragColor;
  gl_FragDepth = rayLengthAndDepth.y;
}

