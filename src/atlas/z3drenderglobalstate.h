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
  // Performance aggregation helpers
  // ---------------------------------------------------------------------------
  // A monotonically increasing token that identifies a user-visible render
  // frame (one engine-driven filter pipeline evaluation). This identity is
  // required even when performance collection is disabled: Vulkan uses it to
  // keep asynchronous presentation monotonic and to select frame-local PPLL
  // resources. The perf collector reuses the same token when enabled.
  uint64_t currentRenderFrameToken() const
  {
    return m_currentRenderFrameToken;
  }

  std::chrono::steady_clock::time_point currentPerfFrameStartTime() const
  {
    return m_currentPerfFrameStartTime;
  }

  // Allocate a monotonically increasing submission ID within the current
  // render-frame token. Vulkan uses this identity for submission-local cache
  // ownership even when performance collection is disabled; the perf collector
  // reuses it for stable ordering when enabled.
  uint32_t nextRenderFrameSubmissionId(uint64_t token);

  // Start a new render-frame token. Capturing the perf start timestamp is
  // optional so `--atlas_perf_mode=off` retains correctness sequencing without
  // paying for collector timing.
  uint64_t beginNewRenderFrameToken(bool collectPerf)
  {
    ++m_currentRenderFrameToken;
    m_currentPerfFrameStartTime =
      collectPerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    m_currentRenderFrameSubmissionCursor = 0;
    return m_currentRenderFrameToken;
  }

private:
  Z3DRenderGlobalState();

  // Cancellation requests may come from other threads (e.g. UI-driven object removal) while the render thread is
  // mid-frame. Guard access to the source pointer to avoid data races.
  mutable std::mutex m_cancellationMutex;
  std::shared_ptr<folly::CancellationSource> m_cancellationSource;
  std::shared_ptr<folly::CancellationSource> m_captureCancellationSource;

  // Monotonically increasing render-frame identity. Performance aggregation
  // consumes this identity but does not own its lifetime.
  uint64_t m_currentRenderFrameToken = 0;
  std::chrono::steady_clock::time_point m_currentPerfFrameStartTime{};
  uint32_t m_currentRenderFrameSubmissionCursor = 0;
};

} // namespace nim
