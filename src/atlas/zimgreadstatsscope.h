#pragma once

#include "zimgreadstats.h"

#include <cstddef>

namespace nim {

// Lightweight thread-local scope for attributing underlying I/O to a 3D paging request.
//
// This is Atlas-only (unlike ZIoReadStatsScope, which lives in src/img/) and is intended for
// components that perform internal disk-cache reads (e.g., SQLite-backed imgregion cache) where
// we cannot easily intercept low-level file reads without instrumenting third-party libraries.
class ZImgReadStatsScope
{
public:
  ZImgReadStatsScope(ZImgReadStatsSink* sink, const ZImgReadStatsContext& ctx);
  ~ZImgReadStatsScope();

  ZImgReadStatsScope(const ZImgReadStatsScope&) = delete;
  ZImgReadStatsScope& operator=(const ZImgReadStatsScope&) = delete;

private:
  ZImgReadStatsSink* m_prevSink = nullptr;
  ZImgReadStatsContext m_prevCtx{};
};

// Reports bytes read from an underlying I/O source to the current thread-local scope (if any).
void reportUnderlyingIoBytesFromImgReadStatsScope(ZImgUnderlyingIoKind kind, size_t bytes);

} // namespace nim

