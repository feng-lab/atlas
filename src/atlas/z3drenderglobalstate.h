#pragma once

#include "z3drendererstates.h"

#include <folly/CancellationToken.h>
#include <memory>

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
  folly::CancellationSource& ensureCancellationSource();
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

private:
  Z3DRenderGlobalState();

  Z3DScratchResourcePool& accessScratchPool() const;

  std::unique_ptr<folly::CancellationSource> m_cancellationSource;
  Z3DScratchResourcePool* m_scratchPool = nullptr;
  RendererSharedState m_rendererState;
};

} // namespace nim
