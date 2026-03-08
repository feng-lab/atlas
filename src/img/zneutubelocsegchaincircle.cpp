#include "zneutubelocsegchaincircle.h"

#include "zneutubelocsegchainknot.h"
#include "zneutubeneuroseg.h"
#include "zswcgeom.h"

#include "zlog.h"

#include <optional>

namespace nim {

namespace {

constexpr int NeurosegCircleRxLegacyLike = 0;

// Port of tz_locseg_chain_knot.c::Locseg_Chain_Knot_Array_To_Circle_Z().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainKnotArrayToCircleZLegacyLike(const LocsegChainKnotArrayLegacyLike& ka,
                                                                               double zToXYRatio,
                                                                               double swellRatio,
                                                                               double swellDiff,
                                                                               double swellLimit)
{
  CHECK(ka.chain != nullptr);

  const int n = static_cast<int>(ka.knots.size());
  std::vector<Geo3dCircle> circles;
  circles.reserve(static_cast<size_t>(n));

  int index = 0;
  int knotIndex = 0;

  for (const auto& node : *ka.chain) {
    LocalNeuroseg locseg2 = node.locseg;
    if (swellRatio != 1.0 || swellDiff != 0.0 || swellLimit > 0.0) {
      neurosegSwellLegacyLike(locseg2.seg, swellRatio, swellDiff, swellLimit);
    }
    if (zToXYRatio != 1.0) {
      localNeurosegScaleZLegacyLike(locseg2, zToXYRatio);
    }

    const LocsegChainKnotLegacyLike* knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
    while (knot != nullptr) {
      if (knot->id == index) {
        circles.push_back(localNeurosegToCircleTLegacyLike(locseg2, knot->offset, NeurosegCircleRxLegacyLike));
        ++knotIndex;
        knot = locsegChainKnotArrayAtLegacyLike(ka, knotIndex);
      } else {
        break;
      }
    }

    ++index;
  }

  CHECK(knotIndex == n) << "Not all knots were converted to circles. knotIndex=" << knotIndex << " n=" << n;
  return circles;
}

} // namespace

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayZLegacyLike(const LocsegChain& chain, double zToXYRatio)
{
  return locsegChainToGeo3dCircleArraySwelledZLegacyLike(chain,
                                                         zToXYRatio,
                                                         /*swellRatio*/ 1.0,
                                                         /*swellDiff*/ 0.0,
                                                         /*swellLimit*/ 0.0);
}

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArraySwelledZLegacyLike(const LocsegChain& chain,
                                                                         double zToXYRatio,
                                                                         double swellRatio,
                                                                         double swellDiff,
                                                                         double swellLimit)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array_Z(), with optional legacy-style swelling
  // applied before circle conversion so traced-exclusion geometry can match the dense trace-mask envelope.
  const auto kaOpt = locsegChainToKnotArrayLegacyLike(chain);
  if (!kaOpt) {
    return {};
  }
  return locsegChainKnotArrayToCircleZLegacyLike(*kaOpt, zToXYRatio, swellRatio, swellDiff, swellLimit);
}

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayLegacyLike(const LocsegChain& chain)
{
  return locsegChainToGeo3dCircleArrayZLegacyLike(chain, 1.0);
}

} // namespace nim
