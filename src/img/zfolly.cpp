#include "zfolly.h"

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/system/HardwareConcurrency.h>

namespace nim {

size_t atlasBackgroundExecutorThreadCount()
{
  const size_t nthreads = folly::available_concurrency();
  return nthreads > 0 ? nthreads : 1;
}

folly::Executor::KeepAlive<> getAtlasBackgroundExecutor()
{
  static folly::CPUThreadPoolExecutor cpuExecutor(
    atlasBackgroundExecutorThreadCount(),
    std::make_shared<folly::NamedThreadFactory>("AtlasBackgroundThreadPool"));
  return folly::getKeepAliveToken(&cpuExecutor);
}

} // namespace nim
