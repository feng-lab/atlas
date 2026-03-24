#pragma once

#include <folly/Executor.h>

namespace nim {

// Dedicated Atlas background executor for user-facing background jobs and
// blocking remote-I/O adapter work. This is a separate pool from Folly's true
// global CPU executor, but it follows the same thread-count policy.
folly::Executor::KeepAlive<> getAtlasBackgroundExecutor();

} // namespace nim
