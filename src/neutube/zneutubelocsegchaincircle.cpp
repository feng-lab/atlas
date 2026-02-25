#include "zneutubelocsegchaincircle.h"

#include "zneutubelocsegchainknot.h"
#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <optional>

namespace nim::neutube {

namespace {

constexpr int NeurosegCircleRxLegacyLike = 0;

// Port of tz_locseg_chain_knot.c::Locseg_Chain_Knot_Array_To_Circle_Z().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainKnotArrayToCircleZLegacyLike(const LocsegChainKnotArrayLegacyLike& ka,
                                                                               double zScale)
{
  CHECK(ka.chain != nullptr);

  const int n = static_cast<int>(ka.knots.size());
  std::vector<Geo3dCircle> circles;
  circles.reserve(static_cast<size_t>(n));

  int index = 0;
  int knotIndex = 0;

  for (const auto& node : *ka.chain) {
    LocalNeuroseg locseg2 = node.locseg;
    if (zScale != 1.0) {
      localNeurosegScaleZLegacyLike(locseg2, zScale);
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

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayZLegacyLike(const LocsegChain& chain, double zScale)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array_Z().
  const auto kaOpt = locsegChainToKnotArrayLegacyLike(chain);
  if (!kaOpt) {
    return {};
  }
  return locsegChainKnotArrayToCircleZLegacyLike(*kaOpt, zScale);
}

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayLegacyLike(const LocsegChain& chain)
{
  return locsegChainToGeo3dCircleArrayZLegacyLike(chain, 1.0);
}

} // namespace nim::neutube
