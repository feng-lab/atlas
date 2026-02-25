#include "zneutubelocsegchainmetrics.h"

#include "zneutubelocsegchaincircle.h"
#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <limits>

namespace nim::neutube {

double locsegChainGeolenLegacyLike(const LocsegChain& chain)
{
  if (chain.empty()) {
    return 0.0;
  }

  const std::vector<Geo3dCircle> circles = locsegChainToGeo3dCircleArrayLegacyLike(chain);
  if (circles.size() < 2) {
    return 0.0;
  }

  double dist = 0.0;
  for (size_t i = 1; i < circles.size(); ++i) {
    const auto& prev = circles[i - 1].center;
    const auto& cur = circles[i].center;
    dist += geo3dDist(prev[0], prev[1], prev[2], cur[0], cur[1], cur[2]);
  }

  return dist;
}

double
locsegChainAverageScoreLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale, StackFitOption option)
{
  if (chain.empty()) {
    return 0.0;
  }

  double score = 0.0;
  int n = 0;

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(option);

  for (const auto& node : chain) {
    score += localNeurosegScorePLegacyLike(node.locseg, stack, zScale, &fs);
    ++n;
  }

  CHECK(n > 0);
  return score / static_cast<double>(n);
}

double
locsegChainMinSegScoreLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale, StackFitOption option)
{
  if (chain.empty()) {
    return 0.0;
  }

  double minScore = std::numeric_limits<double>::infinity();

  StackFitScore fs{};
  fs.n = 1;
  fs.options[0] = static_cast<int>(option);

  for (const auto& node : chain) {
    const double score = localNeurosegScorePLegacyLike(node.locseg, stack, zScale, &fs);
    if (score < minScore) {
      minScore = score;
    }
  }

  return minScore;
}

double locsegChainAverageSignalLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale)
{
  return locsegChainAverageScoreLegacyLike(chain, stack, zScale, StackFitOption::MeanSignal);
}

double locsegChainDistUpperBoundLegacyLike(const LocsegChain& chain, double zScale, const LocalNeuroseg& testseg)
{
  // Port of tz_locseg_chain.c::locseg_chain_dist_upper_bound(chain, z_scale, testseg).
  if (chain.empty()) {
    return 0.0;
  }

  const std::array<double, 3> source = localNeurosegCenterLegacyLike(testseg);

  double minDist = std::numeric_limits<double>::infinity();
  LocalNeuroseg locseg2;

  for (const auto& node : chain) {
    locseg2 = node.locseg;
    localNeurosegScaleZLegacyLike(locseg2, zScale);
    const std::array<double, 3> target = localNeurosegCenterLegacyLike(locseg2);
    const double dist = geo3dDist(source[0], source[1], source[2], target[0], target[1], target[2]);
    if (dist < minDist) {
      minDist = dist;
    }
  }

  if (minDist == std::numeric_limits<double>::infinity()) {
    return 0.0;
  }
  return minDist;
}

} // namespace nim::neutube
