#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/ppll_common.glslinc"

const uint kPPLLSmallListInsertionSortThreshold = 32u;

void ppllSwapFragments(uint a, uint b)
{
  PPLLFragment tmp = ppll_fragments.fragments[a];
  ppll_fragments.fragments[a] = ppll_fragments.fragments[b];
  ppll_fragments.fragments[b] = tmp;
}

// Sift-down for a max-heap by depth (larger depth = farther).
void ppllSiftDown(uint base, uint count, uint root)
{
  uint i = root;
  while (true) {
    const uint left = 2u * i + 1u;
    if (left >= count) {
      break;
    }

    const uint right = left + 1u;
    uint largest = i;

    float largestDepth = ppll_fragments.fragments[base + largest].depth;
    const float leftDepth = ppll_fragments.fragments[base + left].depth;
    if (leftDepth > largestDepth) {
      largest = left;
      largestDepth = leftDepth;
    }
    if (right < count) {
      const float rightDepth = ppll_fragments.fragments[base + right].depth;
      if (rightDepth > largestDepth) {
        largest = right;
      }
    }

    if (largest == i) {
      break;
    }
    ppllSwapFragments(base + i, base + largest);
    i = largest;
  }
}

// In-place heapsort by depth (ascending: near -> far).
// This is exact and avoids the O(n^2) worst-case behavior of insertion sort.
void ppllHeapSortByDepth(uint base, uint count)
{
  if (count <= 1u) {
    return;
  }

  // Build max-heap.
  for (uint i = count / 2u; i > 0u; --i) {
    ppllSiftDown(base, count, i - 1u);
  }

  // Extract max repeatedly to produce ascending order.
  for (uint end = count - 1u; end > 0u; --end) {
    ppllSwapFragments(base, base + end);
    ppllSiftDown(base, end, 0u);
  }
}

void main()
{
  const uint pixelIndex = ppllPixelIndexFromFragCoord(gl_FragCoord.xy);
  if (pixelIndex == 0xffffffffu || pixelIndex >= ppll_params.pixelCount) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  uint count = ppll_counts.counts[pixelIndex];
  if (count == 0u) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  const uint base = ppll_offsets.offsets[pixelIndex];

  // Guard against out-of-bounds reads if offsets/counts are corrupted.
  const uint capacity = ppll_params.fragmentCapacity;
  if (base >= capacity) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }
  const uint maxCount = capacity - base;
  if (count > maxCount) {
    count = maxCount;
  }
  if (count == 0u) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  // For tiny lists, insertion sort is typically faster; for large depth-complexity
  // pixels, heapsort avoids quadratic blow-ups that can trigger GPU timeouts.
  if (count <= kPPLLSmallListInsertionSortThreshold) {
    for (uint i = 1u; i < count; ++i) {
      PPLLFragment key = ppll_fragments.fragments[base + i];
      uint j = i;
      while (j > 0u && ppll_fragments.fragments[base + (j - 1u)].depth > key.depth) {
        ppll_fragments.fragments[base + j] = ppll_fragments.fragments[base + (j - 1u)];
        --j;
      }
      ppll_fragments.fragments[base + j] = key;
    }
  } else {
    ppllHeapSortByDepth(base, count);
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
