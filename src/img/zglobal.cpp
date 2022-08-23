#include "zglobal.h"

#include "zcpuinfo.h"
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace nim {

folly::Executor::KeepAlive<> getGlobalCPUExecutor()
{
  size_t nthreads = ZCpuInfo::instance().nLogicalCores * 2;
  static folly::CPUThreadPoolExecutor cpuExecutor(nthreads,
                                                  16,
                                                  std::make_shared<folly::NamedThreadFactory>("GlobalCPUThreadPool"));
  return folly::getKeepAliveToken(&cpuExecutor);
}

} // namespace nim
