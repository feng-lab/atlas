#include "zneutubelocsegchainknot.h"

#include "zlog.h"

namespace nim::neutube {

namespace {

constexpr double LocsegChainIsEqualEpsLegacyLike = 1e-5;

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

void locsegChainKnotArrayAppendLegacyLike(LocsegChainKnotArrayLegacyLike* ka, LocsegChainKnotLegacyLike knot)
{
  CHECK(ka != nullptr);
  ka->knots.push_back(knot);
}

void locsegChainKnotArrayAppendUniqueLegacyLike(LocsegChainKnotArrayLegacyLike* ka, LocsegChainKnotLegacyLike knot)
{
  CHECK(ka != nullptr);

  const auto* last = locsegChainKnotArrayLastLegacyLike(*ka);
  if (last == nullptr || !locsegChainKnotIsEqualLegacyLike(*last, knot)) {
    ka->knots.push_back(knot);
  }
}

} // namespace nim::neutube
