#include "zrenderthreadexecutor_tls.h"

#include "zlog.h"
#include "zqtexecutor.h"

namespace nim {

namespace {

ZQtExecutor*& renderThreadExecutorSlot()
{
  thread_local ZQtExecutor* executor = nullptr;
  return executor;
}

} // namespace

ZQtExecutor* currentRenderThreadExecutorOrNull()
{
  return renderThreadExecutorSlot();
}

ZQtExecutor& currentRenderThreadExecutor()
{
  ZQtExecutor* executor = renderThreadExecutorSlot();
  CHECK(executor != nullptr) << "Render-thread executor is not set for this thread";
  return *executor;
}

folly::Executor::KeepAlive<> currentRenderThreadExecutorKeepAlive(std::string_view debugLabel)
{
  ZQtExecutor* executor = renderThreadExecutorSlot();
  CHECK(executor != nullptr) << "Render-thread executor is not set for this thread" << (debugLabel.empty() ? "" : " (")
                             << debugLabel << (debugLabel.empty() ? "" : ")");
  return folly::getKeepAliveToken(executor);
}

void setCurrentRenderThreadExecutor(ZQtExecutor* executor)
{
  renderThreadExecutorSlot() = executor;
}

} // namespace nim
