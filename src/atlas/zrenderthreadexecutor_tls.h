#pragma once

#include <folly/Executor.h>

#include <string_view>

namespace nim {

class ZQtExecutor;

// Render-thread executor pointer for the current thread.
//
// This is intentionally thread-local: Atlas runs one render thread per 3D
// window, and engine-owned executors must never be used from other threads.
//
// The rendering engine is responsible for setting this on the render thread
// during initialization and clearing it during teardown.
ZQtExecutor* currentRenderThreadExecutorOrNull();
ZQtExecutor& currentRenderThreadExecutor();
void setCurrentRenderThreadExecutor(ZQtExecutor* executor);

// Convenience helper for coroutine call sites.
//
// Prefer this over manually calling currentRenderThreadExecutorOrNull() +
// folly::getKeepAliveToken(...), so call sites stay concise and all checks are
// consistent.
folly::Executor::KeepAlive<> currentRenderThreadExecutorKeepAlive(std::string_view debugLabel = {});

} // namespace nim
