#include "z3drenderglobalstate.h"

#include "zlog.h"

#include <limits>

namespace nim {
namespace {

struct RenderFrameContext
{
  uint64_t token = 0u;
  std::chrono::steady_clock::time_point perfStartTime{};
  uint32_t submissionCursor = 0u;
};

RenderFrameContext& currentRenderFrameContext()
{
  thread_local RenderFrameContext context;
  return context;
}

} // namespace

Z3DRenderGlobalState& Z3DRenderGlobalState::instance()
{
  static Z3DRenderGlobalState state;
  return state;
}

Z3DRenderGlobalState::Z3DRenderGlobalState() = default;

uint64_t Z3DRenderGlobalState::currentRenderFrameToken() const
{
  return currentRenderFrameContext().token;
}

std::chrono::steady_clock::time_point Z3DRenderGlobalState::currentPerfFrameStartTime() const
{
  return currentRenderFrameContext().perfStartTime;
}

uint32_t Z3DRenderGlobalState::nextRenderFrameSubmissionId(uint64_t token)
{
  auto& context = currentRenderFrameContext();
  CHECK_GT(token, 0u);
  CHECK_EQ(token, context.token) << "Render submission ID requested for a non-current render token. token=" << token
                                 << " current=" << context.token;
  return ++context.submissionCursor;
}

uint64_t Z3DRenderGlobalState::beginNewRenderFrameToken(bool collectPerf)
{
  auto& context = currentRenderFrameContext();
  CHECK_LT(context.token, std::numeric_limits<uint64_t>::max()) << "Render frame token space exhausted";
  ++context.token;
  context.perfStartTime = collectPerf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  context.submissionCursor = 0u;
  return context.token;
}

bool Z3DRenderGlobalState::hasCancellationSource() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  return static_cast<bool>(m_cancellationSource);
}

std::optional<uint64_t> Z3DRenderGlobalState::idleCancellationCheckpoint() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (m_cancellationSource) {
    return std::nullopt;
  }
  return m_cancellationRequestGeneration;
}

std::shared_ptr<folly::CancellationSource> Z3DRenderGlobalState::tryAcquireCancellationSource(uint64_t checkpoint)
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (m_cancellationSource || m_cancellationRequestGeneration != checkpoint) {
    return nullptr;
  }
  m_cancellationSource = std::make_shared<folly::CancellationSource>();
  return m_cancellationSource;
}

void Z3DRenderGlobalState::releaseCancellationSource(const std::shared_ptr<folly::CancellationSource>& source)
{
  CHECK(source);
  const std::scoped_lock lock(m_cancellationMutex);
  CHECK(m_cancellationSource == source) << "Only the render that acquired a cancellation source may release it";
  m_cancellationSource.reset();
}

void Z3DRenderGlobalState::requestCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_cancellationMutex);
    CHECK_LT(m_cancellationRequestGeneration, std::numeric_limits<uint64_t>::max())
      << "Render cancellation request generation exhausted";
    ++m_cancellationRequestGeneration;
    source = m_cancellationSource;
  }
  if (source) {
    source->requestCancellation();
  }
}

std::shared_ptr<folly::CancellationSource> Z3DRenderGlobalState::ensureCaptureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (!m_captureCancellationSource) {
    m_captureCancellationSource = std::make_shared<folly::CancellationSource>();
  }
  return m_captureCancellationSource;
}

void Z3DRenderGlobalState::resetCaptureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  m_captureCancellationSource.reset();
}

void Z3DRenderGlobalState::requestCaptureCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_cancellationMutex);
    source = m_captureCancellationSource;
  }
  if (source) {
    source->requestCancellation();
  }
}

folly::CancellationToken Z3DRenderGlobalState::currentCancellationToken() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (m_cancellationSource && m_captureCancellationSource) {
    return folly::cancellation_token_merge(m_cancellationSource->getToken(), m_captureCancellationSource->getToken());
  }
  if (m_cancellationSource) {
    return m_cancellationSource->getToken();
  }
  if (m_captureCancellationSource) {
    return m_captureCancellationSource->getToken();
  }
  return folly::CancellationToken();
}

} // namespace nim
