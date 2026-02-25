#include "zneutubetracemask.h"

#include "zneutubeimgbinarizer.h"
#include "zneutubeimgbwmorph.h"

#include "zlog.h"

namespace nim::neutube {

std::optional<ZImg> makeMaskLegacyLike(const ZImg& stack, const TraceConfig& cfg, MakeMaskDiagnosticsLegacyLike* diag)
{
  if (stack.isEmpty()) {
    return std::nullopt;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (cfg.enhanceMask) {
    // Legacy supports this via `enhanceLine()` + a second LOCMAX binarization pass.
    // We intentionally fail fast here until the exact enhancement pipeline is migrated.
    LOG(ERROR) << "makeMaskLegacyLike: enhanceMask=true is not supported yet in neutube command2.";
    return std::nullopt;
  }

  // Legacy `ZNeuronTracer::binarize()` behavior (ZStackBinarizer LOCMAX, retry count 3).
  const BinarizeResultLegacyLike bin = binarizeLocmaxLegacyLike(stack, /*retryCount*/ 3);
  if (!bin.success) {
    return std::nullopt;
  }

  if (diag != nullptr) {
    diag->binarizeThreshold = bin.actualThreshold;
  }

  // Legacy `bwsolid(bw)` currently only applies a majority filter R.
  constexpr int mnbr = 4;
  ZImg mask = majorityFilterBinaryU8RLegacyLike(bin.binary, /*connectivity*/ 26, /*mnbr*/ mnbr);

  return mask;
}

} // namespace nim::neutube
