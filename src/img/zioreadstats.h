#pragma once

#include <cstddef>
#include <cstdint>

namespace nim {

// Lightweight thread-local scope for attributing low-level file reads to a higher-level operation.
//
// This is intentionally decoupled from Atlas renderer types to avoid introducing dependencies
// from the img library onto atlas. Callers provide callbacks and an opaque user pointer.
//
// Typical usage (Atlas side):
// - Install a ZIoReadStatsScope in the worker thread that performs a tile/chunk read.
// - Low-level file helpers (readStream_impl, memory-mapped reads, etc.) call reportFileReadBytes().
// - The callback forwards bytes to a per-frame stats sink using the (channel, round) context.
struct ZIoReadStatsContext
{
  std::uint32_t channel = 0;
  std::uint32_t round = 0;
};

using ZIoReadStatsBytesFn = void (*)(void* user, const ZIoReadStatsContext& ctx, size_t bytes);

struct ZIoReadStatsHooks
{
  void* user = nullptr;
  ZIoReadStatsBytesFn onFileReadBytes = nullptr;
};

class ZIoReadStatsScope
{
public:
  ZIoReadStatsScope(const ZIoReadStatsHooks& hooks, const ZIoReadStatsContext& ctx);
  ~ZIoReadStatsScope();

  ZIoReadStatsScope(const ZIoReadStatsScope&) = delete;
  ZIoReadStatsScope& operator=(const ZIoReadStatsScope&) = delete;

private:
  ZIoReadStatsHooks m_prevHooks;
  ZIoReadStatsContext m_prevCtx;
};

// Reports bytes read from a file-like source to the current thread-local scope (if any).
void reportFileReadBytes(size_t bytes);

} // namespace nim

