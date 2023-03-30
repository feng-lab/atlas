#include "zfolly.h"

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <thread>

namespace nim {

folly::Executor::KeepAlive<> getGlobalCPUExecutor()
{
  static folly::CPUThreadPoolExecutor cpuExecutor(std::thread::hardware_concurrency(),
                                                  8,
                                                  std::make_shared<folly::NamedThreadFactory>("GlobalCPUThreadPool"));
  return folly::getKeepAliveToken(&cpuExecutor);
}

} // namespace nim
