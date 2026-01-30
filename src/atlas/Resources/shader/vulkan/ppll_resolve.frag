#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/ppll_common.glslinc"

void main()
{
  const uint pixelIndex = ppllPixelIndexFromFragCoord(gl_FragCoord.xy);
  if (pixelIndex == 0xffffffffu || pixelIndex >= ppll_params.pixelCount) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  const uint count = ppll_counts.counts[pixelIndex];
  if (count == 0u) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  const uint base = ppll_offsets.offsets[pixelIndex];

  // In-place insertion sort by depth (ascending: near -> far).
  // This is exact and does not impose a hard per-pixel fragment cap.
  for (uint i = 1u; i < count; ++i) {
    PPLLFragment key = ppll_fragments.fragments[base + i];
    uint j = i;
    while (j > 0u && ppll_fragments.fragments[base + (j - 1u)].depth > key.depth) {
      ppll_fragments.fragments[base + j] = ppll_fragments.fragments[base + (j - 1u)];
      --j;
    }
    ppll_fragments.fragments[base + j] = key;
  }

  vec3 accumRgb = vec3(0.0);
  float accumAlpha = 0.0;
  for (uint i = 0u; i < count; ++i) {
    const PPLLFragment frag = ppll_fragments.fragments[base + i];
    const float oneMinusA = 1.0 - accumAlpha;
    accumRgb += frag.color.rgb * oneMinusA;
    accumAlpha += frag.color.a * oneMinusA;
  }

  FragData0 = vec4(accumRgb, accumAlpha);
  // Nearest depth (first element after sort)
  gl_FragDepth = ppll_fragments.fragments[base].depth;
}

