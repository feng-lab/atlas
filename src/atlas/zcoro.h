#pragma once

#include "zexception.h"
#include "zlog.h"

#include <folly/OperationCancelled.h>
#include <folly/coro/Task.h>

#include <string>
#include <string_view>

namespace nim {

// Start a coroutine task that has been bound to an executor (via co_withExecutor),
// and enforce a consistent exception policy for detached tasks.
//
// Why this exists:
// - Folly's TaskWithExecutor::start() without a callback returns a SemiFuture.
//   Many engine call sites are "fire-and-forget" and otherwise drop the result,
//   which can silently swallow exceptions and turn bugs into late, hard-to-debug
//   crashes (often on the UI thread).
//
// Policy:
// - ZCancellationException / folly::OperationCancelled: treated as normal cancellation.
// - Any other exception: fatal (engine invariant violation).
template<typename T>
inline void startCoroTaskChecked(folly::coro::TaskWithExecutor<T>&& task, std::string_view debugLabel = {})
{
  std::string label(debugLabel);
  std::move(task).start([label = std::move(label)](auto&& result) mutable {
    if (!result.hasException()) {
      return;
    }

    if (result.template hasException<ZCancellationException>() ||
        result.template hasException<folly::OperationCancelled>()) {
      return;
    }

    const auto& ex = result.exception();
    LOG(FATAL) << "Detached coroutine task failed" << (label.empty() ? "" : " label='") << (label.empty() ? "" : label)
               << (label.empty() ? "" : "'") << ": " << ex.what();
  });
}

} // namespace nim
