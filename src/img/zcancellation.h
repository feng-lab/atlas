#pragma once

#include "zglobal.h"
#include "zexception.h"
#include <QCoreApplication>
#include <folly/CancellationToken.h>

namespace nim {

__forceinline void processEventsAndMaybeCancel(const folly::CancellationToken& cancellationToken)
{
  if (cancellationToken.canBeCancelled()) {
    if (!cancellationToken.isCancellationRequested()) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    if (cancellationToken.isCancellationRequested()) {
      throw ZCancellationException();
    }
  }
}

__forceinline void maybeCancel(const folly::CancellationToken& cancellationToken)
{
  if (cancellationToken.isCancellationRequested()) {
    throw ZCancellationException();
  }
}

} // namespace nim
