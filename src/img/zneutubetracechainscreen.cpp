#include "zneutubetracechainscreen.h"

#include "zneutubelocsegchainmetrics.h"

#include "zlog.h"

#include <limits>

namespace nim {

void screenChainsLegacyLike(const ZImg& signal, std::vector<std::unique_ptr<LocsegChain>>& chains)
{
  if (chains.empty()) {
    return;
  }

  constexpr double scoreThreshold = 0.6;
  constexpr double zToXYRatio = 1.0;

  std::vector<double> scoreArray(chains.size(), 0.0);
  std::vector<double> intensityArray(chains.size(), 0.0);

  double minIntensity = std::numeric_limits<double>::infinity();

  for (size_t i = 0; i < chains.size(); ++i) {
    const LocsegChain* chain = chains[i].get();
    CHECK(chain != nullptr);
    scoreArray[i] = locsegChainAverageScoreLegacyLike(*chain, signal, zToXYRatio, StackFitOption::Corrcoef);
    intensityArray[i] = locsegChainAverageSignalLegacyLike(*chain, signal, zToXYRatio);
    if (scoreArray[i] >= scoreThreshold) {
      if (intensityArray[i] < minIntensity) {
        minIntensity = intensityArray[i];
      }
    }
  }

  std::vector<std::unique_ptr<LocsegChain>> good;
  good.reserve(chains.size());

  for (size_t i = 0; i < chains.size(); ++i) {
    if (scoreArray[i] >= scoreThreshold || intensityArray[i] >= minIntensity) {
      good.push_back(std::move(chains[i]));
    }
  }

  chains = std::move(good);
}

} // namespace nim
