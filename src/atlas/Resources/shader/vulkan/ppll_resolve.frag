#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 FragData0;

#include "include/bindless.glslinc"

layout(push_constant) uniform PPLLResolvePC {
  uint opaque_depth_texture;
} pc;

#include "include/ppll_common.glslinc"

// Algorithm crossover only: every visible fragment is retained. Insertion sort
// is efficient for short lists and heapsort bounds the work for long lists.
const uint kPPLLSmallListInsertionSortThreshold = 32u;

// Invalid depths are excluded from resolve rather than entering an unordered
// NaN comparison path.
bool ppllDepthIsValid(float depth)
{
  return !isnan(depth) && !isinf(depth) && depth >= 0.0 && depth <= 1.0;
}

int ppllCompareRecordIndices(uint base, uint lhsRecordIndex, uint rhsRecordIndex)
{
  const uint lhsIndex = base + lhsRecordIndex;
  const uint rhsIndex = base + rhsRecordIndex;
  const float lhsDepth = ppll_fragments.fragments[lhsIndex].depth;
  const float rhsDepth = ppll_fragments.fragments[rhsIndex].depth;
  if (lhsDepth < rhsDepth) {
    return -1;
  }
  if (rhsDepth < lhsDepth) {
    return 1;
  }

  // Exact equal-depth fragments have no scene-order key in the PPLL record, so
  // their compositing order is intentionally unspecified.
  return 0;
}

uint ppllLoadSortIndex(uint base, uint slot)
{
  return ppll_fragments.fragments[base + slot].sortIndex;
}

void ppllStoreSortIndex(uint base, uint slot, uint recordIndex)
{
  ppll_fragments.fragments[base + slot].sortIndex = recordIndex;
}

void ppllInsertionSortRecordIndices(uint base, uint count)
{
  for (uint i = 1u; i < count; ++i) {
    const uint keyRecordIndex = ppllLoadSortIndex(base, i);
    uint j = i;
    while (j > 0u) {
      const uint previousRecordIndex = ppllLoadSortIndex(base, j - 1u);
      if (ppllCompareRecordIndices(base, previousRecordIndex, keyRecordIndex) <= 0) {
        break;
      }
      ppllStoreSortIndex(base, j, previousRecordIndex);
      --j;
    }
    ppllStoreSortIndex(base, j, keyRecordIndex);
  }
}

// The semantic color/depth records remain immutable. Heapsort mutates only
// scalar index workspace and keeps large depth-complexity lists O(n log n).
void ppllSiftDownSortIndices(uint base, uint count, uint root, uint rootRecordIndex)
{
  uint i = root;
  const uint firstLeaf = count / 2u;
  while (i < firstLeaf) {
    const uint left = 2u * i + 1u;
    const uint right = left + 1u;
    uint largestChild = left;
    uint largestChildRecordIndex = ppllLoadSortIndex(base, left);
    if (right < count) {
      const uint rightRecordIndex = ppllLoadSortIndex(base, right);
      if (ppllCompareRecordIndices(base, largestChildRecordIndex, rightRecordIndex) < 0) {
        largestChild = right;
        largestChildRecordIndex = rightRecordIndex;
      }
    }

    if (ppllCompareRecordIndices(base, rootRecordIndex, largestChildRecordIndex) >= 0) {
      break;
    }
    ppllStoreSortIndex(base, i, largestChildRecordIndex);
    i = largestChild;
  }
  ppllStoreSortIndex(base, i, rootRecordIndex);
}

void ppllHeapSortRecordIndices(uint base, uint count)
{
  for (uint i = count / 2u; i > 0u; --i) {
    const uint root = i - 1u;
    ppllSiftDownSortIndices(base, count, root, ppllLoadSortIndex(base, root));
  }

  for (uint end = count - 1u; end > 0u; --end) {
    const uint maxRecordIndex = ppllLoadSortIndex(base, 0u);
    const uint replacementRecordIndex = ppllLoadSortIndex(base, end);
    ppllStoreSortIndex(base, end, maxRecordIndex);
    ppllSiftDownSortIndices(base, end, 0u, replacementRecordIndex);
  }
}

void ppllCompositeFragment(vec4 color, inout vec3 accumRgb, inout float accumAlpha)
{
  const float oneMinusA = 1.0 - accumAlpha;
  accumRgb += color.rgb * oneMinusA;
  accumAlpha += color.a * oneMinusA;
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

  // Opaque depth at this pixel. Any transparent fragments with depth >= opaqueDepth are occluded and should not
  // contribute to the resolved transparent layer. This mirrors the intended "depth-test against opaque" behavior
  // of the count/store passes, without relying on early fragment tests (SSBO writes can force late depth tests).
  //
  // Note: when the compositor binds a 1x1 placeholder texture (no opaque pass), texelFetch() must clamp to avoid
  // undefined out-of-bounds reads for screen-sized fragment coordinates.
  float opaqueDepth = 1.0;
  const ivec2 opaqueDepthSize = textureSize(atlas_bindlessSampler2DNearest(pc.opaque_depth_texture), 0);
  if (opaqueDepthSize.x > 0 && opaqueDepthSize.y > 0) {
    const ivec2 clampedCoord =
      clamp(ivec2(gl_FragCoord.xy), ivec2(0), opaqueDepthSize - ivec2(1));
    opaqueDepth = texelFetch(atlas_bindlessSampler2DNearest(pc.opaque_depth_texture), clampedCoord, 0).r;
  }
  if (!ppllDepthIsValid(opaqueDepth)) {
    opaqueDepth = 1.0;
  }

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

  uint visibleCount = 0u;
  for (uint recordIndex = 0u; recordIndex < count; ++recordIndex) {
    const float depth = ppll_fragments.fragments[base + recordIndex].depth;
    if (!ppllDepthIsValid(depth) || depth >= opaqueDepth) {
      continue;
    }
    ppllStoreSortIndex(base, visibleCount, recordIndex);
    ++visibleCount;
  }
  if (visibleCount == 0u) {
    FragData0 = vec4(0.0);
    gl_FragDepth = 1.0;
    return;
  }

  // Sort only scalar indices; color and depth stay immutable. Every valid
  // visible fragment remains represented exactly once in the workspace.
  if (visibleCount <= kPPLLSmallListInsertionSortThreshold) {
    ppllInsertionSortRecordIndices(base, visibleCount);
  } else {
    ppllHeapSortRecordIndices(base, visibleCount);
  }

  vec3 accumRgb = vec3(0.0);
  float accumAlpha = 0.0;
  for (uint slot = 0u; slot < visibleCount; ++slot) {
    const uint recordIndex = ppllLoadSortIndex(base, slot);
    ppllCompositeFragment(ppll_fragments.fragments[base + recordIndex].color,
                          accumRgb,
                          accumAlpha);
  }

  FragData0 = vec4(accumRgb, accumAlpha);
  const uint nearestRecordIndex = ppllLoadSortIndex(base, 0u);
  gl_FragDepth = ppll_fragments.fragments[base + nearestRecordIndex].depth;
}
