#include "zneutubetracemask.h"

#include "zneutubeimgbinarizer.h"
#include "zneutubeimgbwmorph.h"

#include "zlog.h"

#include <chrono>

namespace nim {

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
  const bool collectTiming = (diag != nullptr) && diag->collectTiming;
  if (diag != nullptr) {
    diag->binarizeDiag.collectTiming = collectTiming;
  }

  const auto t0 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const BinarizeResultLegacyLike bin =
    binarizeLocmaxLegacyLike(stack, /*retryCount*/ 3, (diag != nullptr) ? &diag->binarizeDiag : nullptr);
  if (collectTiming) {
    diag->ms_binarize_locmax =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  }
  if (!bin.success) {
    return std::nullopt;
  }

  if (diag != nullptr) {
    diag->binarizeThreshold = bin.actualThreshold;
  }

  // Legacy `bwsolid(bw)` currently only applies a majority filter R.
  constexpr int mnbr = 4;
  const auto t1 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  ZImg mask = majorityFilterBinaryU8RLegacyLike(bin.binary, /*connectivity*/ 26, /*mnbr*/ mnbr);
  if (collectTiming) {
    diag->ms_bwsolid_majority_filter_r =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  }

  return mask;
}

} // namespace nim
