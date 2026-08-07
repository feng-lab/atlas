#pragma once

#include "zfolly.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace nim {

class Z3DRenderGlobalState
{
public:
  static Z3DRenderGlobalState& instance();

  bool hasCancellationSource() const;
  std::shared_ptr<folly::CancellationSource> ensureCancellationSource();
  void resetCancellationSource();
  void requestCancellation();
  std::shared_ptr<folly::CancellationSource> ensureCaptureCancellationSource();
  void resetCaptureCancellationSource();
  void requestCaptureCancellation();
  folly::CancellationToken currentCancellationToken() const;

  // ---------------------------------------------------------------------------
  // Render-frame identity and performance aggregation
  // ---------------------------------------------------------------------------
  // A rendering-thread-local, monotonically increasing token that identifies
  // one engine-driven filter pipeline evaluation. This identity is required
  // even when performance collection is disabled: each Vulkan backend uses it
  // for monotonic asynchronous presentation and PPLL ring advancement. The
  // thread-local perf collector reuses the same token when enabled.
  uint64_t currentRenderFrameToken() const;

  std::chrono::steady_clock::time_point currentPerfFrameStartTime() const;

  // Allocate a monotonically increasing submission ID within the current
  // render-frame token. Vulkan uses this identity for submission-local cache
  // ownership even when performance collection is disabled; the perf collector
  // reuses it for stable ordering when enabled.
  uint32_t nextRenderFrameSubmissionId(uint64_t token);

  // Start a new render-frame token. Capturing the perf start timestamp is
  // optional so `--atlas_perf_mode=off` retains correctness sequencing without
  // paying for collector timing.
  uint64_t beginNewRenderFrameToken(bool collectPerf);

private:
  Z3DRenderGlobalState();

  // Cancellation requests may come from other threads (e.g. UI-driven object removal) while the render thread is
  // mid-frame. Guard access to the source pointer to avoid data races.
  mutable std::mutex m_cancellationMutex;
  std::shared_ptr<folly::CancellationSource> m_cancellationSource;
  std::shared_ptr<folly::CancellationSource> m_captureCancellationSource;
};

} // namespace nim
