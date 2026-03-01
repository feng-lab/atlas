#include "zneutubelocsegchainknot.h"

#include "zswcgeom.h"

#include "zlog.h"

namespace nim {

namespace {

constexpr double LocsegChainIsEqualEpsLegacyLike = 1e-5;
constexpr double LocsegChainToKnotArrayEpsLegacyLike = 1e-3;

} // namespace

bool locsegChainKnotIsEqualLegacyLike(const LocsegChainKnotLegacyLike& knot1, const LocsegChainKnotLegacyLike& knot2)
{
  // Port of tz_locseg_chain_knot.c::Locseg_Chain_Knot_Is_Equal().
  return knot1.id == knot2.id && std::fabs(knot1.offset - knot2.offset) < LocsegChainIsEqualEpsLegacyLike;
}

const LocsegChainKnotLegacyLike* locsegChainKnotArrayAtLegacyLike(const LocsegChainKnotArrayLegacyLike& ka, int index)
{
  if (index < 0 || index >= static_cast<int>(ka.knots.size())) {
    return nullptr;
  }
  return &ka.knots[static_cast<size_t>(index)];
}

const LocsegChainKnotLegacyLike* locsegChainKnotArrayLastLegacyLike(const LocsegChainKnotArrayLegacyLike& ka)
{
  return locsegChainKnotArrayAtLegacyLike(ka, static_cast<int>(ka.knots.size()) - 1);
}

void locsegChainKnotArrayAppendLegacyLike(LocsegChainKnotArrayLegacyLike& ka, LocsegChainKnotLegacyLike knot)
{
  ka.knots.push_back(knot);
}

void locsegChainKnotArrayAppendUniqueLegacyLike(LocsegChainKnotArrayLegacyLike& ka, LocsegChainKnotLegacyLike knot)
{
  const auto* last = locsegChainKnotArrayLastLegacyLike(ka);
  if (last == nullptr || !locsegChainKnotIsEqualLegacyLike(*last, knot)) {
    ka.knots.push_back(knot);
  }
}

std::optional<LocsegChainKnotArrayLegacyLike> locsegChainToKnotArrayLegacyLike(const LocsegChain& chain)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_To_Knot_Array().
  const int length = chain.length();
  if (length == 0) {
    return std::nullopt;
  }

  LocsegChainKnotArrayLegacyLike ka;
  ka.chain = &chain;

  locsegChainKnotArrayAppendLegacyLike(ka, LocsegChainKnotLegacyLike{0, 0.0});

  if (length == 1) {
    locsegChainKnotArrayAppendLegacyLike(ka, LocsegChainKnotLegacyLike{0, 1.0});
    return ka;
  }

  int index = 0;
  const LocsegNode* prevNode = nullptr;
  const LocsegNode* lastNode = nullptr;

  for (const auto& node : chain) {
    double offset = -1.0;

    if (traceRecordDirection(node.tr) != TraceDirection::BothDir) {
      TraceDirection traceDirection = TraceDirection::Forward;
      offset = traceRecordFixPoint(node.tr);
      traceDirection = traceRecordDirection(node.tr);

      if (offset < 0.0) {
        switch (traceDirection) {
          case TraceDirection::Forward:
            offset = 0.0;
            if (prevNode != nullptr) {
              if (traceRecordDirection(prevNode->tr) == TraceDirection::Backward) {
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
        locsegChainKnotArrayAppendUniqueLegacyLike(ka, LocsegChainKnotLegacyLike{index, offset});
        lastNode = &node;
      }
    }

    prevNode = &node;
    ++index;
  }

  locsegChainKnotArrayAppendUniqueLegacyLike(ka, LocsegChainKnotLegacyLike{length - 1, 1.0});
  return ka;
}

std::array<double, 3> locsegChainKnotPosLegacyLike(const LocsegChainKnotArrayLegacyLike& ka, int index)
{
  // Port of tz_locseg_chain_knot.c::Locseg_Chain_Knot_Pos().
  CHECK(ka.chain != nullptr);
  const auto* knot = locsegChainKnotArrayAtLegacyLike(ka, index);
  CHECK(knot != nullptr) << "Invalid knot index: " << index;

  const int knotId = knot->id;
  CHECK(knotId >= 0);
  CHECK(knotId < ka.chain->length());

  const LocsegNode* node = nullptr;
  int i = 0;
  for (const auto& cur : *ka.chain) {
    if (i == knotId) {
      node = &cur;
      break;
    }
    ++i;
  }
  CHECK(node != nullptr);

  return localNeurosegAxisCoordNLegacyLike(node->locseg, knot->offset);
}

} // namespace nim
