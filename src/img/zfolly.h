#pragma once

// Central Folly gateway for Atlas headers. zlog.h must come first so Folly's
// vendored glog include sites cannot replace Atlas' Abseil LOG/CHECK macros.
#include "zlog.h"

#include <cstddef>

#include <folly/CancellationToken.h>
#include <folly/Executor.h>
#include <folly/Function.h>
#include <folly/OperationCancelled.h>
#include <folly/ScopeGuard.h>
#include <folly/ThreadLocal.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/io/async/ScopedEventBaseThread.h>

namespace nim {

[[nodiscard]] size_t atlasBackgroundExecutorThreadCount();

// Dedicated Atlas background executor for user-facing background jobs and
// blocking remote-I/O adapter work. This is a separate pool from Folly's true
// global CPU executor and sizes itself from folly::available_concurrency().
folly::Executor::KeepAlive<> getAtlasBackgroundExecutor();

} // namespace nim
