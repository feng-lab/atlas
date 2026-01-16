#include "zioreadstats.h"

namespace nim {

namespace {

thread_local ZIoReadStatsHooks g_tlsHooks{};
thread_local ZIoReadStatsContext g_tlsCtx{};

} // namespace

ZIoReadStatsScope::ZIoReadStatsScope(const ZIoReadStatsHooks& hooks, const ZIoReadStatsContext& ctx)
  : m_prevHooks(g_tlsHooks)
  , m_prevCtx(g_tlsCtx)
{
  g_tlsHooks = hooks;
  g_tlsCtx = ctx;
}

ZIoReadStatsScope::~ZIoReadStatsScope()
{
  g_tlsHooks = m_prevHooks;
  g_tlsCtx = m_prevCtx;
}

void reportFileReadBytes(size_t bytes)
{
  if (bytes == 0) {
    return;
  }
  if (!g_tlsHooks.onFileReadBytes) {
    return;
  }
  g_tlsHooks.onFileReadBytes(g_tlsHooks.user, g_tlsCtx, bytes);
}

} // namespace nim

