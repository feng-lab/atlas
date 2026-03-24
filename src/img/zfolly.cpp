#include "zfolly.h"

#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/system/HardwareConcurrency.h>

namespace nim {

namespace {

[[nodiscard]] size_t atlasBackgroundExecutorThreadCount()
{
  const uint32_t configuredThreads = FLAGS_folly_global_cpu_executor_threads;
  size_t nthreads = configuredThreads > 0 ? static_cast<size_t>(configuredThreads) : folly::hardware_concurrency();
  if (nthreads == 0) {
    nthreads = 1;
  }
  return nthreads;
}

} // namespace

folly::Executor::KeepAlive<> getAtlasBackgroundExecutor()
{
  static folly::CPUThreadPoolExecutor cpuExecutor(
    atlasBackgroundExecutorThreadCount(),
    std::make_shared<folly::NamedThreadFactory>("AtlasBackgroundThreadPool"));
  return folly::getKeepAliveToken(&cpuExecutor);
}

} // namespace nim
