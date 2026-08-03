#include "z3drenderglobalstate.h"

#include "zlog.h"

namespace nim {

Z3DRenderGlobalState& Z3DRenderGlobalState::instance()
{
  static Z3DRenderGlobalState state;
  return state;
}

Z3DRenderGlobalState::Z3DRenderGlobalState() = default;

uint32_t Z3DRenderGlobalState::nextRenderFrameSubmissionId(uint64_t token)
{
  CHECK_GT(token, 0u);
  CHECK_EQ(token, m_currentRenderFrameToken)
    << "Render submission ID requested for a non-current render token. token=" << token
    << " current=" << m_currentRenderFrameToken;
  return ++m_currentRenderFrameSubmissionCursor;
}

bool Z3DRenderGlobalState::hasCancellationSource() const
{
  const std::scoped_lock lock(m_cancellationMutex);
  return static_cast<bool>(m_cancellationSource);
}

std::shared_ptr<folly::CancellationSource> Z3DRenderGlobalState::ensureCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  if (!m_cancellationSource) {
    m_cancellationSource = std::make_shared<folly::CancellationSource>();
  }
  return m_cancellationSource;
}

void Z3DRenderGlobalState::resetCancellationSource()
{
  const std::scoped_lock lock(m_cancellationMutex);
  m_cancellationSource.reset();
}

void Z3DRenderGlobalState::requestCancellation()
{
  std::shared_ptr<folly::CancellationSource> source;
  {
    const std::scoped_lock lock(m_cancellationMutex);
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
