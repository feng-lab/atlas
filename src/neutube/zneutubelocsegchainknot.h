#pragma once

#include "zneutubelocsegchain.h"

#include <cmath>
#include <vector>

namespace nim::neutube {

// C++ port of tz_locseg_chain_knot.h::Locseg_Chain_Knot.
struct LocsegChainKnotLegacyLike
{
  int id = -1;
  double offset = 0.0;
};

// C++ port of tz_locseg_chain_knot.h::Locseg_Chain_Knot_Array.
struct LocsegChainKnotArrayLegacyLike
{
  const LocsegChain* chain = nullptr;
  std::vector<LocsegChainKnotLegacyLike> knots;
};

[[nodiscard]] bool locsegChainKnotIsEqualLegacyLike(const LocsegChainKnotLegacyLike& knot1,
                                                    const LocsegChainKnotLegacyLike& knot2);

[[nodiscard]] const LocsegChainKnotLegacyLike*
locsegChainKnotArrayAtLegacyLike(const LocsegChainKnotArrayLegacyLike& ka, int index);

[[nodiscard]] const LocsegChainKnotLegacyLike*
locsegChainKnotArrayLastLegacyLike(const LocsegChainKnotArrayLegacyLike& ka);

void locsegChainKnotArrayAppendLegacyLike(LocsegChainKnotArrayLegacyLike* ka, LocsegChainKnotLegacyLike knot);

void locsegChainKnotArrayAppendUniqueLegacyLike(LocsegChainKnotArrayLegacyLike* ka, LocsegChainKnotLegacyLike knot);

} // namespace nim::neutube
