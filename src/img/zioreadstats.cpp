#include "zioreadstats.h"

namespace nim {

namespace {

struct IoReadStatsTlsState
{
  ZIoReadStatsHooks hooks;
  ZIoReadStatsContext ctx;
};

IoReadStatsTlsState& ioReadStatsTlsState()
{
  thread_local IoReadStatsTlsState state;
  return state;
}

} // namespace

ZIoReadStatsScope::ZIoReadStatsScope(const ZIoReadStatsHooks& hooks, const ZIoReadStatsContext& ctx)
  : m_prevHooks(ioReadStatsTlsState().hooks)
  , m_prevCtx(ioReadStatsTlsState().ctx)
{
  auto& state = ioReadStatsTlsState();
  state.hooks = hooks;
  state.ctx = ctx;
}

ZIoReadStatsScope::~ZIoReadStatsScope()
{
  auto& state = ioReadStatsTlsState();
  state.hooks = m_prevHooks;
  state.ctx = m_prevCtx;
}

void reportFileReadBytes(size_t bytes)
{
  if (bytes == 0) {
    return;
  }
  auto& state = ioReadStatsTlsState();
  if (!state.hooks.onFileReadBytes) {
    return;
  }
  state.hooks.onFileReadBytes(state.hooks.user, state.ctx, bytes);
}

} // namespace nim
