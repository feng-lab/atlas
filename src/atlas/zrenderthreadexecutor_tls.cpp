#include "zrenderthreadexecutor_tls.h"

#include "zlog.h"
#include "zqtexecutor.h"

namespace nim {

thread_local ZQtExecutor* g_renderThreadExecutor = nullptr;

ZQtExecutor* currentRenderThreadExecutorOrNull()
{
  return g_renderThreadExecutor;
}

ZQtExecutor& currentRenderThreadExecutor()
{
  CHECK(g_renderThreadExecutor != nullptr) << "Render-thread executor is not set for this thread";
  return *g_renderThreadExecutor;
}

folly::Executor::KeepAlive<> currentRenderThreadExecutorKeepAlive(std::string_view debugLabel)
{
  CHECK(g_renderThreadExecutor != nullptr)
    << "Render-thread executor is not set for this thread" << (debugLabel.empty() ? "" : " (") << debugLabel
    << (debugLabel.empty() ? "" : ")");
  return folly::getKeepAliveToken(g_renderThreadExecutor);
}

void setCurrentRenderThreadExecutor(ZQtExecutor* executor)
{
  g_renderThreadExecutor = executor;
}

} // namespace nim
