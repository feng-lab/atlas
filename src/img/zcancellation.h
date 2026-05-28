#pragma once

#include "zglobal.h"
#include "zexception.h"
#include "zfolly.h"

namespace nim {

__forceinline void maybeCancel(const folly::CancellationToken& cancellationToken)
{
  if (cancellationToken.isCancellationRequested()) {
    throw ZCancellationException();
  }
}

} // namespace nim
