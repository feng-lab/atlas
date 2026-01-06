#include "z3dfilter.h"

#include "zlog.h"
#include "zsysteminfo.h"
#include "z3dinteractionhandler.h"
#include "zeventlistenerparameter.h"
#include "zparameter.h"

namespace nim {

Z3DFilter::Z3DFilter(QObject* parent)
  : QObject(parent)
  , m_state(State::AllResultInvalid)
{}

ZParameter* Z3DFilter::parameter(const QString& name) const
{
  for (auto para : m_parameters) {
    if (para->name() == name) {
      return para;
    }
  }
  return nullptr;
}

void Z3DFilter::invalidate(State inv)
{
  // VLOG(1) << className() << " 1";
  CHECK(inv != State::Valid);
  if (isFlagSet(m_state, inv)) {
    return;
  }
  // VLOG(1) << className() << " 2";

  setFlag(m_state, inv);
  Q_EMIT invalidated();
}

void Z3DFilter::onEvent(QEvent* e, int w, int h)
{
  e->ignore();

  // LOG(WARNING) << e << " " << className();
  //  propagate to interaction handlers
  for (size_t i = 0; i < m_interactionHandlers.size() && !e->isAccepted(); ++i) {
    m_interactionHandlers[i]->onEvent(e, w, h);
  }

  // propagate to event listeners
  for (size_t i = 0; (i < m_eventListeners.size()) && !e->isAccepted(); ++i) {
    m_eventListeners[i]->sendEvent(e, w, h);
  }
}

void Z3DFilter::read(const json::object& json)
{
  for (auto para : m_parameters) {
    para->read(json);
  }
}

void Z3DFilter::write(json::object& json) const
{
  for (auto para : m_parameters) {
    para->write(json);
  }
}

void Z3DFilter::setValid(Z3DEye eye)
{
  // VLOG(1) << className() << " setValid(" << eye << ")";
  if (eye == MonoEye) {
    unsetFlag(m_state, State::MonoViewResultInvalid);
  } else if (eye == LeftEye) {
    unsetFlag(m_state, State::LeftEyeResultInvalid);
  } else {
    unsetFlag(m_state, State::RightEyeResultInvalid);
  }
}

bool Z3DFilter::isValid(Z3DEye eye) const
{
  if (eye == MonoEye) {
    return !isFlagSet(m_state, State::MonoViewResultInvalid);
  } else if (eye == LeftEye) {
    return !isFlagSet(m_state, State::LeftEyeResultInvalid);
  } else {
    return !isFlagSet(m_state, State::RightEyeResultInvalid);
  }
}

bool Z3DFilter::isReady(Z3DEye) const
{
  // Readiness is driven by per-filter overrides (visibility, data presence)
  return true;
}

void Z3DFilter::addParameter(ZParameter& para, State inv)
{
  if (m_parameterNames.count(para.name())) {
    LOG(FATAL) << "Duplicated para name " << para.name();
  }
  m_parameters.push_back(&para);
  m_parameterNames.insert(para.name());
  if (inv != State::Valid) {
#ifdef NO // ATLAS_DEBUG_VERSION
    // Capture and tag parameter changes (debug-only) before invalidation.
    connect(&para, &ZParameter::valueChanged, this, [this, &para]() {
      try {
        QString valueStr = nim::jsonToFormattedQString(para.jsonValue());
        debugSetInvalidateReason(QString("parameter '%1' changed to %2").arg(para.name(), valueStr));
      }
      catch (...) {
        debugSetInvalidateReason(QString("parameter '%1' changed").arg(para.name()));
      }
      invalidateResult();
    });
#else
    connect(&para, &ZParameter::valueChanged, this, &Z3DFilter::invalidateResult);
#endif
  }
}

void Z3DFilter::removeParameter(ZParameter& para)
{
  if (!m_parameterNames.count(para.name())) {
    LOG(ERROR) << className() << " parameter " << para.name() << " cannot be removed, it does not exist";
  }
  para.disconnect(this);
  std::erase(m_parameters, &para);
  m_parameterNames.erase(para.name());
}

void Z3DFilter::addEventListener(ZEventListenerParameter& para)
{
  addParameter(para);
  m_eventListeners.push_back(&para);
}

void Z3DFilter::addInteractionHandler(Z3DInteractionHandler& handler)
{
  m_interactionHandlers.push_back(&handler);
}

void Z3DFilter::updateSize(const glm::uvec2& targetSize)
{
  if (targetSize == m_lastUpdateSize) {
    return;
  }
  m_lastUpdateSize = targetSize;

  // Provide a reason so downstream logs can attribute this invalidation.
#ifdef NO // ATLAS_DEBUG_VERSION
  debugSetInvalidateReason("updateSize");
#endif
  invalidate(State::AllResultInvalid);
}

} // namespace nim
