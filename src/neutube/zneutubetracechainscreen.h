#pragma once

#include "zneutubelocsegchain.h"

#include "zimg.h"

#include <memory>
#include <vector>

namespace nim::neutube {

// Port of `ZNeuronTracer::screenChain(...)`.
//
// Removes low-quality chains based on:
// - average corrcoef score threshold (0.6),
// - and an adaptive intensity threshold based on the minimum average signal among high-score chains.
void screenChainsLegacyLike(const ZImg& signal, std::vector<std::unique_ptr<LocsegChain>>* chains);

} // namespace nim::neutube
