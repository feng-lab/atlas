#include "zneutubetraceseeder.h"

#include "zneutubestackfitoptions.h"
#include "zneutubetracelocseglabel.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

[[nodiscard]] ZImg makeBaseMaskLike(const ZImg& signal)
{
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);
  ZImgInfo info(signal.width(), signal.height(), signal.depth(), 1, 1, 1, VoxelFormat::Unsigned);
  ZImg out(info);
  return out;
}

[[nodiscard]] int maskValueAt(const ZImg& mask, int x, int y, int z)
{
  const int width = static_cast<int>(mask.width());
  const int height = static_cast<int>(mask.height());
  const int depth = static_cast<int>(mask.depth());
  if (x < 0 || y < 0 || z < 0 || x >= width || y >= height || z >= depth) {
    return 0;
  }

  return imgTypeDispatcher(mask.info(), [&]<typename TVoxel>() -> int {
    return static_cast<int>(*mask.data<TVoxel>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
  });
}

} // namespace

SeedSortResultLegacyLike sortSeedsLegacyLike(const Geo3dScalarField& seeds, const ZImg& signal, TraceWorkspace& tw)
{
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);

  SeedSortResultLegacyLike out;
  out.locsegArray.resize(seeds.size());
  out.scoreArray.assign(seeds.size(), 0.0);
  out.baseMask = makeBaseMaskLike(signal);

  // Configure fit workspace like legacy ZNeuronTraceSeeder::sortSeed.
  tw.fitWorkspace.sws.fs.n = 2;
  tw.fitWorkspace.sws.fs.options[0] = static_cast<int>(StackFitOption::Dot);
  tw.fitWorkspace.sws.fs.options[1] = static_cast<int>(StackFitOption::Corrcoef);
  tw.fitWorkspace.posAdjust = 1;

  const size_t width = out.baseMask.width();
  const size_t height = out.baseMask.height();
  const size_t depth = out.baseMask.depth();
  const size_t plane = width * height;

  auto* baseMaskData = out.baseMask.timeData<uint8_t>(0);

  for (size_t i = 0; i < seeds.size(); ++i) {
    const int x = static_cast<int>(seeds.points[i][0]);
    const int y = static_cast<int>(seeds.points[i][1]);
    const int z = static_cast<int>(seeds.points[i][2]);

    if (tw.traceMask) {
      const int v = maskValueAt(*tw.traceMask, x, y, z);
      if (v > 0) {
        out.scoreArray[i] = 0.0;
        continue;
      }
    }

    double widthValue = seeds.values[i];
    if (widthValue < 3.0) {
      widthValue += 0.5;
    }

    if (x < 0 || y < 0 || z < 0 || static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height ||
        static_cast<size_t>(z) >= depth) {
      out.scoreArray[i] = 0.0;
      continue;
    }

    const size_t seedOffset = static_cast<size_t>(x) + static_cast<size_t>(y) * width + static_cast<size_t>(z) * plane;
    if (baseMaskData[seedOffset] > 0) {
      out.scoreArray[i] = 0.0;
      continue;
    }

    LocalNeuroseg& seed = out.locsegArray[i];
    seed.seg.r1 = widthValue;
    seed.seg.c = 0.0;
    seed.seg.h = NeurosegDefaultHLegacyLike;
    seed.seg.theta = 0.0;
    seed.seg.psi = 0.0;
    seed.seg.curvature = 0.0;
    seed.seg.alpha = 0.0;
    seed.seg.scale = 1.0;

    const std::array<double, 3> cpos = {static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)};
    setNeurosegPositionLegacyLike(seed, cpos, NeuroposReferenceLegacyLike::Center);

    constexpr double zScale = 1.0;
    (void)localNeurosegOptimizeWLegacyLike(seed, signal, zScale, /*option*/ 0, tw.fitWorkspace);

    if (tw.traceMask) {
      if (localNeurosegHitMaskLegacyLike(seed, *tw.traceMask)) {
        out.scoreArray[i] = 0.0;
        continue;
      }
    }

    out.scoreArray[i] = tw.fitWorkspace.sws.fs.scores[1];

    const int labelValue = (out.scoreArray[i] > tw.minScore) ? 2 : 1;
    localNeurosegLabelGLegacyLike(seed, out.baseMask, /*flag*/ -1, labelValue, zScale);
  }

  return out;
}

} // namespace nim
