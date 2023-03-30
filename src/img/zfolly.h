#pragma once

#include <folly/Executor.h>

namespace nim {

folly::Executor::KeepAlive<> getGlobalCPUExecutor();

} // namespace nim
