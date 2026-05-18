#pragma once

#include <cstddef>

#include <folly/Executor.h>

namespace nim {

[[nodiscard]] size_t atlasBackgroundExecutorThreadCount();

// Dedicated Atlas background executor for user-facing background jobs and
// blocking remote-I/O adapter work. This is a separate pool from Folly's true
// global CPU executor and sizes itself from folly::available_concurrency().
folly::Executor::KeepAlive<> getAtlasBackgroundExecutor();

} // namespace nim
