#include "zimgreadstatsscope.h"

namespace nim {

namespace {

thread_local ZImgReadStatsSink* g_tlsSink = nullptr;
thread_local ZImgReadStatsContext g_tlsCtx{};

} // namespace

ZImgReadStatsScope::ZImgReadStatsScope(ZImgReadStatsSink* sink, const ZImgReadStatsContext& ctx)
  : m_prevSink(g_tlsSink)
  , m_prevCtx(g_tlsCtx)
{
  g_tlsSink = sink;
  g_tlsCtx = ctx;
}

ZImgReadStatsScope::~ZImgReadStatsScope()
{
  g_tlsSink = m_prevSink;
  g_tlsCtx = m_prevCtx;
}

void reportUnderlyingIoBytesFromImgReadStatsScope(ZImgUnderlyingIoKind kind, size_t bytes)
{
  if (bytes == 0) {
    return;
  }
  if (!g_tlsSink) {
    return;
  }
  g_tlsSink->onUnderlyingIoBytes(g_tlsCtx, kind, bytes);
}

} // namespace nim

