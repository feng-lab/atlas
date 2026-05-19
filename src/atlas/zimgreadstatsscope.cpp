#include "zimgreadstatsscope.h"

namespace nim {

namespace {

struct ImgReadStatsTlsState
{
  ZImgReadStatsSink* sink = nullptr;
  ZImgReadStatsContext ctx;
};

ImgReadStatsTlsState& imgReadStatsTlsState()
{
  thread_local ImgReadStatsTlsState state;
  return state;
}

} // namespace

ZImgReadStatsScope::ZImgReadStatsScope(ZImgReadStatsSink* sink, const ZImgReadStatsContext& ctx)
  : m_prevSink(imgReadStatsTlsState().sink)
  , m_prevCtx(imgReadStatsTlsState().ctx)
{
  auto& state = imgReadStatsTlsState();
  state.sink = sink;
  state.ctx = ctx;
}

ZImgReadStatsScope::~ZImgReadStatsScope()
{
  auto& state = imgReadStatsTlsState();
  state.sink = m_prevSink;
  state.ctx = m_prevCtx;
}

void reportUnderlyingIoBytesFromImgReadStatsScope(ZImgUnderlyingIoKind kind, size_t bytes)
{
  if (bytes == 0) {
    return;
  }
  auto& state = imgReadStatsTlsState();
  if (!state.sink) {
    return;
  }
  state.sink->onUnderlyingIoBytes(state.ctx, kind, bytes);
}

} // namespace nim
