#version 450

// Required for includes and bindless tables
#extension GL_GOOGLE_include_directive : require

#include "include/bindless.glslinc"

// Composition modes
// 0=DEPTH_TEST, 1=FIRST_ON_TOP, 2=SECOND_ON_TOP,
// 3=MIP_IMAGE_DEPTH_TEST_BLENDING, 4=DEPTH_TEST_BLENDING,
// 5=FIRST_ON_TOP_BLENDING, 6=SECOND_ON_TOP_BLENDING
layout(constant_id = 60) const int COMPOSE_MODE = 0;

layout(push_constant) uniform CompositorPC {
  vec2 screen_dim_RCP;
  uint color_texture_0;
  uint depth_texture_0;
  uint color_texture_1;
  uint depth_texture_1;
} cpc;

layout(location = 0) out vec4 FragData0;

void main()
{
  vec2 texCoords = gl_FragCoord.xy * cpc.screen_dim_RCP;

  vec4 color0 = texture(atlas_bindlessSampler2DLinear(cpc.color_texture_0), texCoords);
  float depth0 = texture(atlas_bindlessSampler2DLinear(cpc.depth_texture_0), texCoords).r;
  vec4 color1 = texture(atlas_bindlessSampler2DLinear(cpc.color_texture_1), texCoords);
  float depth1 = texture(atlas_bindlessSampler2DLinear(cpc.depth_texture_1), texCoords).r;

  if (COMPOSE_MODE == 0) { // DEPTH_TEST
    if (depth0 < depth1) { FragData0 = color0; gl_FragDepth = depth0; }
    else { FragData0 = color1; gl_FragDepth = depth1; }
  } else if (COMPOSE_MODE == 1) { // FIRST_ON_TOP
    if (color0.a > 0.0) { FragData0 = color0; gl_FragDepth = depth0; }
    else if (color1.a > 0.0) { FragData0 = color1; gl_FragDepth = depth1; }
    else { discard; }
  } else if (COMPOSE_MODE == 2) { // SECOND_ON_TOP
    if (color1.a > 0.0) { FragData0 = color1; gl_FragDepth = depth1; }
    else if (color0.a > 0.0) { FragData0 = color0; gl_FragDepth = depth0; }
    else { discard; }
  } else if (COMPOSE_MODE == 3) { // MIP_IMAGE_DEPTH_TEST_BLENDING
    if (all(equal(color0, vec4(0, 0, 0, 1)))) {
      FragData0 = color1; gl_FragDepth = depth1;
    } else if (all(equal(color1, vec4(0, 0, 0, 1)))) {
      FragData0 = color0; gl_FragDepth = depth0;
    } else if (depth1 < depth0) {
      FragData0 = color1 + (1.0 - color1.a) * color0; gl_FragDepth = depth1;
    } else {
      FragData0 = color0 + (1.0 - color0.a) * color1; gl_FragDepth = depth0;
    }
  } else if (COMPOSE_MODE == 4) { // DEPTH_TEST_BLENDING
    if (depth1 < depth0) { FragData0 = color1 + (1.0 - color1.a) * color0; gl_FragDepth = depth1; }
    else { FragData0 = color0 + (1.0 - color0.a) * color1; gl_FragDepth = depth0; }
  } else if (COMPOSE_MODE == 5) { // FIRST_ON_TOP_BLENDING
    FragData0 = color0 + (1.0 - color0.a) * color1;
    gl_FragDepth = color0.a > 0.0 ? depth0 : depth1;
  } else if (COMPOSE_MODE == 6) { // SECOND_ON_TOP_BLENDING
    FragData0 = color1 + (1.0 - color1.a) * color0;
    gl_FragDepth = color1.a > 0.0 ? depth1 : depth0;
  } else {
    discard;
  }
}
