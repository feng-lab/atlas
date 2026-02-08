#pragma once

#include "z3drendererstates.h"

#include <chrono>
#include <folly/CancellationToken.h>
#include <memory>
#include <mutex>

namespace nim {

class Z3DScratchResourcePool;
class Z3DCamera;
class Z3DGlobalParameters;

class Z3DRenderGlobalState
{
public:
  static Z3DRenderGlobalState& instance();

  Z3DScratchResourcePool& scratchPool();
  void trimScratchPool();

  bool hasScratchPool() const;
  void setScratchPool(Z3DScratchResourcePool* pool);
  void resetScratchPool();

  bool hasCancellationSource() const;
  std::shared_ptr<folly::CancellationSource> ensureCancellationSource();
  void resetCancellationSource();
  void requestCancellation();
  folly::CancellationToken currentCancellationToken() const;

  struct RendererSharedState
  {
    RendererViewState viewState;
    RendererSceneState sceneState;
    bool viewDirty = true;
    bool sceneDirty = true;
  };

  RendererSharedState& rendererState()
  {
    return m_rendererState;
  }

  const RendererSharedState& rendererState() const
  {
    return m_rendererState;
  }

  void markViewStateDirty();
  void markSceneStateDirty();

  void ensureViewState(const Z3DCamera& camera);
  void ensureSceneState(const Z3DGlobalParameters& params);

  // ---------------------------------------------------------------------------
  // Performance aggregation helpers
  // ---------------------------------------------------------------------------
  // A monotonically increasing token that identifies a user‑visible "real frame".
  // Emitted once per engine-driven rendering pass (one filter pipeline
  // evaluation) and propagated to the Vulkan backend so multiple submissions
  // can be aggregated.
  uint64_t currentPerfFrameToken() const
  {
    return m_currentPerfFrameToken;
  }

  std::chrono::steady_clock::time_point currentPerfFrameStartTime() const
  {
    return m_currentPerfFrameStartTime;
  }

  // Start a new perf frame token (increments by 1).
  uint64_t beginNewPerfFrameToken()
  {
    ++m_currentPerfFrameToken;
    m_currentPerfFrameStartTime = std::chrono::steady_clock::now();
    return m_currentPerfFrameToken;
  }

private:
  Z3DRenderGlobalState();

  Z3DScratchResourcePool& accessScratchPool() const;

  // Cancellation requests may come from other threads (e.g. UI-driven object removal) while the render thread is
  // mid-frame. Guard access to the source pointer to avoid data races.
  mutable std::mutex m_cancellationMutex;
  std::shared_ptr<folly::CancellationSource> m_cancellationSource;
  Z3DScratchResourcePool* m_scratchPool = nullptr;
  RendererSharedState m_rendererState;

  // Monotonically increasing identifier for performance aggregation.
  uint64_t m_currentPerfFrameToken = 0;
  std::chrono::steady_clock::time_point m_currentPerfFrameStartTime{};
};

} // namespace nim
