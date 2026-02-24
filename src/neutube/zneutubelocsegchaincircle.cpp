#include "zneutubelocsegchaincircle.h"

#include "zneutubelocsegchainknot.h"
#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <optional>

namespace nim::neutube {

namespace {

constexpr double LocsegChainToKnotArrayEpsLegacyLike = 1e-3;
constexpr int NeurosegCircleRxLegacyLike = 0;

// Port of tz_locseg_chain.c::Locseg_Chain_To_Knot_Array().
[[nodiscard]] std::optional<LocsegChainKnotArrayLegacyLike> locsegChainToKnotArrayLegacyLike(const LocsegChain& chain)
{
  const int length = chain.length();
  if (length == 0) {
    return std::nullopt;
  }

  LocsegChainKnotArrayLegacyLike ka;
  ka.chain = &chain;

  locsegChainKnotArrayAppendLegacyLike(&ka, LocsegChainKnotLegacyLike{0, 0.0});

  if (length == 1) {
    locsegChainKnotArrayAppendLegacyLike(&ka, LocsegChainKnotLegacyLike{0, 1.0});
    return ka;
  }

  int index = 0;
  const LocsegNode* prevNode = nullptr;
  const LocsegNode* lastNode = nullptr;

  for (const auto& node : chain) {
    double offset = -1.0;

    if (traceRecordDirection(&node.tr) != TraceDirection::BothDir) {
      TraceDirection traceDirection = TraceDirection::Forward;
      offset = traceRecordFixPoint(&node.tr);
      traceDirection = traceRecordDirection(&node.tr);

      if (offset < 0.0) {
        switch (traceDirection) {
          case TraceDirection::Forward:
            offset = 0.0;
            if (prevNode != nullptr) {
              if (traceRecordDirection(&prevNode->tr) == TraceDirection::Backward) {
                offset = -1.0;
              }
            }
            break;
          case TraceDirection::Backward:
            offset = 1.0;
            break;
          default:
            offset = 0.0;
            break;
        }
      }
    }

    if (offset >= 0.0) {
      const std::array<double, 3> pos = localNeurosegAxisCoordNLegacyLike(node.locseg, offset);

      double dist = 10.0;
      if (lastNode != nullptr) {
        const auto* lastKnot = locsegChainKnotArrayLastLegacyLike(ka);
        CHECK(lastKnot != nullptr);

        const std::array<double, 3> prevPos = localNeurosegAxisCoordNLegacyLike(lastNode->locseg, lastKnot->offset);
        dist = geo3dDist(prevPos[0], prevPos[1], prevPos[2], pos[0], pos[1], pos[2]);
      }

      if (dist > LocsegChainToKnotArrayEpsLegacyLike) {
        locsegChainKnotArrayAppendUniqueLegacyLike(&ka, LocsegChainKnotLegacyLike{index, offset});
        lastNode = &node;
      }
    }

    prevNode = &node;
    ++index;
  }

  locsegChainKnotArrayAppendUniqueLegacyLike(&ka, LocsegChainKnotLegacyLike{length - 1, 1.0});
  return ka;
}

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
      localNeurosegScaleZLegacyLike(&locseg2, zScale);
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
  const auto ka = locsegChainToKnotArrayLegacyLike(chain);
  if (!ka) {
    return {};
  }
  return locsegChainKnotArrayToCircleZLegacyLike(*ka, zScale);
}

std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayLegacyLike(const LocsegChain& chain)
{
  return locsegChainToGeo3dCircleArrayZLegacyLike(chain, 1.0);
}

} // namespace nim::neutube
