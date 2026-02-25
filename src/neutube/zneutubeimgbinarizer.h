#pragma once

#include "zimg.h"

#include <optional>

namespace nim::neutube {

struct BinarizeResultLegacyLike
{
  ZImg binary; // GREY (uint8) with values 0/1
  bool success = false;
  int actualThreshold = 0;
};

// Port of ZStackProcessor::SubtractBackground(Stack*, double, int).
//
// Returns the subtracted background intensity (0 means no-op), matching legacy.
int subtractBackgroundLegacyLike(ZImg& stack, double minFr, int maxIter);

// Port of ZNeuronTracer::binarize(const Stack*, Stack*).
//
// Returns a GREY (uint8) binary stack (values 0/1) on success.
BinarizeResultLegacyLike binarizeLocmaxLegacyLike(const ZImg& stack, int retryCount);

} // namespace nim::neutube
