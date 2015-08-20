#include "z3dprocessor.h"

#include "QsLog.h"
#include "zsysteminfo.h"
#include "z3dport.h"
#include "z3dinteractionhandler.h"
#include "zparameter.h"
#include "zeventlistenerparameter.h"
#include <cassert>

namespace nim {

Z3DProcessor::Z3DProcessor(QObject *parent)
  : QObject(parent)
  , m_invalidationState(InvalidAllResult)
  , m_invalidationVisited(false)
{
}

Z3DProcessor::~Z3DProcessor()
{
}

void Z3DProcessor::addPort(Z3DInputPortBase &port)
{
  port.setProcessor(this);

  m_inputPorts.push_back(&port);

  std::map<QString, Z3DInputPortBase*>::const_iterator it = m_inputPortMap.find(port.name());
  if (it == m_inputPortMap.end())
    m_inputPortMap.emplace(port.name(), &port);
  else {
    LERROR() << className() << "port" << port.name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DProcessor::addPort(Z3DOutputPortBase &port)
{
  port.setProcessor(this);
  m_outputPorts.push_back(&port);
  std::map<QString, Z3DOutputPortBase*>::const_iterator it = m_outputPortMap.find(port.name());
  if (it == m_outputPortMap.end())
    m_outputPortMap.emplace(port.name(), &port);
  else {
    LERROR() << className() << "port" << port.name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DProcessor::removePort(Z3DInputPortBase &port)
{
  m_inputPorts.erase(std::find(m_inputPorts.begin(), m_inputPorts.end(), &port));

  std::map<QString, Z3DInputPortBase*>::iterator inIt = m_inputPortMap.find(port.name());
  if (inIt != m_inputPortMap.end())
    m_inputPortMap.erase(inIt);
  else {
    LERROR() << className() << "port" << port.name() << "was not found!";
    assert(false);
  }
}

void Z3DProcessor::removePort(Z3DOutputPortBase &port)
{
  m_outputPorts.erase(std::find(m_outputPorts.begin(), m_outputPorts.end(), &port));

  std::map<QString, Z3DOutputPortBase*>::iterator outIt = m_outputPortMap.find(port.name());
  if (outIt != m_outputPortMap.end())
    m_outputPortMap.erase(outIt);
  else {
    LERROR() << className() << "port" << port.name() << "was not found!";
    assert(false);
  }
}

void Z3DProcessor::addParameter(ZParameter &para, InvalidationState inv)
{
  if (m_parameterNames.find(para.name()) != m_parameterNames.end()) {
    LFATAL() << "Duplicated para name" << para.name();
  }
  m_parameters.push_back(&para);
  m_parameterNames.insert(para.name());
  if (inv != Valid) {
    connect(&para, SIGNAL(valueChanged()), this, SLOT(invalidateResult()));
  }
}

void Z3DProcessor::removeParameter(ZParameter &para)
{
  if (!parameter(para.name())) {
    LERROR() << className() << "parameter" << para.name() << "cannot be removed, it does not exist";
  } else {
    para.disconnect(this);
    m_parameters.erase(std::find(m_parameters.begin(), m_parameters.end(), &para));
    m_parameterNames.erase(para.name());
  }
}

ZParameter* Z3DProcessor::parameter(const QString &name) const
{
  for (size_t i=0; i<m_parameters.size(); i++) {
    if (m_parameters[i]->name() == name)
      return m_parameters[i];
  }
  return NULL;
}

bool Z3DProcessor::isInInteractionMode() const
{
  return (!m_interactionModeSources.empty());
}

Z3DInputPortBase *Z3DProcessor::inputPort(const QString &name) const
{
  for (size_t i=0; i < m_inputPorts.size(); i++) {
    if (m_inputPorts[i]->name() == name)
      return m_inputPorts[i];
  }

  return NULL;
}

Z3DOutputPortBase *Z3DProcessor::outputPort(const QString &name) const
{
  for (size_t i=0; i < m_outputPorts.size(); i++) {
    if (m_outputPorts[i]->name() == name)
      return m_outputPorts[i];
  }

  return NULL;
}

void Z3DProcessor::invalidate(InvalidationState inv)
{
  m_invalidationState |= inv;

  if (inv == Z3DProcessor::Valid)
    return;

  if (!m_invalidationVisited) {
    m_invalidationVisited = true;

    for (size_t i=0; i<m_outputPorts.size(); ++i)
      m_outputPorts[i]->invalidate();

    m_invalidationVisited = false;
  }
}

bool Z3DProcessor::isReady(Z3DEye) const
{
  for(size_t i=0; i<m_inputPorts.size(); ++i)
    if (!m_inputPorts[i]->isReady())
      return false;

  for (size_t i=0; i<m_outputPorts.size(); ++i)
    if(!m_outputPorts[i]->isReady())
      return false;

  return true;
}

void Z3DProcessor::toggleInteractionMode(bool interactionMode, void* source)
{
  if (interactionMode) {
    if (m_interactionModeSources.find(source) == m_interactionModeSources.end()) {

      m_interactionModeSources.insert(source);

      if (m_interactionModeSources.size() == 1)
        enterInteractionMode();
    }
  } else {
    if (m_interactionModeSources.find(source) != m_interactionModeSources.end()) {

      m_interactionModeSources.erase(source);

      if (m_interactionModeSources.empty())
        exitInteractionMode();
    }
  }
}

void Z3DProcessor::setValid(Z3DEye eye)
{
  if (eye == Z3DEye::Mono)
    m_invalidationState &= ~InvalidMonoViewResult;
  else if (eye == Z3DEye::Left)
    m_invalidationState &= ~InvalidLeftEyeResult;
  else
    m_invalidationState &= ~InvalidRightEyeResult;

  for (size_t i=0; i<m_inputPorts.size(); ++i)
    m_inputPorts[i]->setValid();
}

bool Z3DProcessor::isValid(Z3DEye eye) const
{
  if (eye == Z3DEye::Mono)
    return !m_invalidationState.testFlag(InvalidMonoViewResult);
  else if (eye == Z3DEye::Left)
    return !m_invalidationState.testFlag(InvalidLeftEyeResult);
  else
    return !m_invalidationState.testFlag(InvalidRightEyeResult);
}

void Z3DProcessor::addEventListener(ZEventListenerParameter &para)
{
  addParameter(para);
  m_eventListeners.push_back(&para);
}

void Z3DProcessor::addInteractionHandler(Z3DInteractionHandler &handler)
{
  m_interactionHandlers.push_back(&handler);
}

void Z3DProcessor::onEvent(QEvent *e, int w, int h)
{
  e->ignore();

  //LWARN() << e << className();
  // propagate to interaction handlers
  for (size_t i=0; i<m_interactionHandlers.size() && !e->isAccepted(); ++i) {
    for (size_t j=0; j<m_interactionHandlers[i]->eventListeners().size() &&
         !e->isAccepted(); ++j) {
      m_interactionHandlers[i]->eventListeners().at(j)->sendEvent(e, w, h);
    }
  }

  // propagate to event listeners
  for (size_t i = 0; (i < m_eventListeners.size()) && !e->isAccepted(); ++i)
    m_eventListeners[i]->sendEvent(e, w, h);
}

void Z3DProcessor::disconnectAllPorts()
{
  for (size_t i = 0; i < m_inputPorts.size(); ++i) {
    m_inputPorts[i]->disconnectAll();
  }

  for (size_t i = 0; i < m_outputPorts.size(); ++i) {
    m_outputPorts[i]->disconnectAll();
  }
}

void Z3DProcessor::read(const QJsonObject &json)
{
  for (size_t i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->read(json);
  }
}

void Z3DProcessor::write(QJsonObject &json) const
{
  for (size_t i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->write(json);
  }
}

} // namespace nim
