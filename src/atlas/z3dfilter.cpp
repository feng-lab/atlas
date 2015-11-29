#include "z3dfilter.h"

#include "QsLog.h"
#include "zsysteminfo.h"
#include "z3dport.h"
#include "z3dinteractionhandler.h"
#include "zparameter.h"
#include "zeventlistenerparameter.h"
#include <cassert>
#include "z3dshaderprogram.h"
#include "z3drenderport.h"
#include "zvertexarrayobject.h"

namespace nim {

Z3DFilter::Z3DFilter(QObject *parent)
  : QObject(parent)
  , m_invalidationState(InvalidAllResult)
  , m_invalidationVisited(false)
{
}

Z3DFilter::~Z3DFilter()
{
}

ZParameter* Z3DFilter::parameter(const QString &name) const
{
  for (size_t i=0; i<m_parameters.size(); i++) {
    if (m_parameters[i]->name() == name)
      return m_parameters[i];
  }
  return NULL;
}

void Z3DFilter::invalidate(InvalidationState inv)
{
  m_invalidationState |= inv;

  if (inv == Z3DFilter::Valid)
    return;

  if (!m_invalidationVisited) {
    m_invalidationVisited = true;

    for (size_t i=0; i<m_outputPorts.size(); ++i)
      m_outputPorts[i]->invalidate();

    m_invalidationVisited = false;
  }
}

Z3DInputPortBase *Z3DFilter::inputPort(const QString &name) const
{
  for (size_t i=0; i < m_inputPorts.size(); i++) {
    if (m_inputPorts[i]->name() == name)
      return m_inputPorts[i];
  }

  return NULL;
}

Z3DOutputPortBase *Z3DFilter::outputPort(const QString &name) const
{
  for (size_t i=0; i < m_outputPorts.size(); i++) {
    if (m_outputPorts[i]->name() == name)
      return m_outputPorts[i];
  }

  return NULL;
}

void Z3DFilter::onEvent(QEvent *e, int w, int h)
{
  e->ignore();

  //LWARN() << e << className();
  // propagate to interaction handlers
  for (size_t i=0; i<m_interactionHandlers.size() && !e->isAccepted(); ++i) {
    m_interactionHandlers[i]->onEvent(e, w, h);
  }

  // propagate to event listeners
  for (size_t i = 0; (i < m_eventListeners.size()) && !e->isAccepted(); ++i)
    m_eventListeners[i]->sendEvent(e, w, h);
}

void Z3DFilter::disconnectAllPorts()
{
  for (size_t i = 0; i < m_inputPorts.size(); ++i) {
    m_inputPorts[i]->disconnectAll();
  }

  for (size_t i = 0; i < m_outputPorts.size(); ++i) {
    m_outputPorts[i]->disconnectAll();
  }
}

void Z3DFilter::read(const QJsonObject &json)
{
  for (size_t i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->read(json);
  }
}

void Z3DFilter::write(QJsonObject &json) const
{
  for (size_t i=0; i<m_parameters.size(); ++i) {
    m_parameters[i]->write(json);
  }
}

void Z3DFilter::setValid(Z3DEye eye)
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

bool Z3DFilter::isValid(Z3DEye eye) const
{
  if (eye == Z3DEye::Mono)
    return !m_invalidationState.testFlag(InvalidMonoViewResult);
  else if (eye == Z3DEye::Left)
    return !m_invalidationState.testFlag(InvalidLeftEyeResult);
  else
    return !m_invalidationState.testFlag(InvalidRightEyeResult);
}

bool Z3DFilter::isReady(Z3DEye) const
{
  for(size_t i=0; i<m_inputPorts.size(); ++i)
    if (!m_inputPorts[i]->isReady())
      return false;

  for (size_t i=0; i<m_outputPorts.size(); ++i)
    if(!m_outputPorts[i]->isReady())
      return false;

  return true;
}

void Z3DFilter::addPort(Z3DInputPortBase &port)
{
  port.setFilter(this);

  m_inputPorts.push_back(&port);

  std::map<QString, Z3DInputPortBase*>::const_iterator it = m_inputPortMap.find(port.name());
  if (it == m_inputPortMap.end())
    m_inputPortMap.emplace(port.name(), &port);
  else {
    LERROR() << className() << "port" << port.name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DFilter::addPort(Z3DOutputPortBase &port)
{
  port.setFilter(this);
  m_outputPorts.push_back(&port);
  std::map<QString, Z3DOutputPortBase*>::const_iterator it = m_outputPortMap.find(port.name());
  if (it == m_outputPortMap.end())
    m_outputPortMap.emplace(port.name(), &port);
  else {
    LERROR() << className() << "port" << port.name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DFilter::removePort(Z3DInputPortBase &port)
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

void Z3DFilter::removePort(Z3DOutputPortBase &port)
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

void Z3DFilter::addParameter(ZParameter &para, InvalidationState inv)
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

void Z3DFilter::removeParameter(ZParameter &para)
{
  if (!parameter(para.name())) {
    LERROR() << className() << "parameter" << para.name() << "cannot be removed, it does not exist";
  } else {
    para.disconnect(this);
    m_parameters.erase(std::find(m_parameters.begin(), m_parameters.end(), &para));
    m_parameterNames.erase(para.name());
  }
}

void Z3DFilter::addEventListener(ZEventListenerParameter &para)
{
  addParameter(para);
  m_eventListeners.push_back(&para);
}

void Z3DFilter::addInteractionHandler(Z3DInteractionHandler &handler)
{
  m_interactionHandlers.push_back(&handler);
}

bool Z3DFilter::isInInteractionMode() const
{
  return (!m_interactionModeSources.empty());
}

void Z3DFilter::toggleInteractionMode(bool interactionMode, void* source)
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

void Z3DFilter::addPrivateRenderPort(Z3DRenderOutputPort& port)
{
  port.setFilter(this);
  m_privateRenderPorts.push_back(&port);

  std::map<QString, Z3DOutputPortBase*>::const_iterator it = m_outputPortMap.find(port.name());
  if (it == m_outputPortMap.end())
    m_outputPortMap.emplace(port.name(), &port);
  else {
    LERROR() << className() << "port" << port.name() << "has already been inserted!";
    assert(false);
  }
}

void Z3DFilter::addPrivateRenderTarget(Z3DRenderTarget &target)
{
  m_privateRenderTargets.push_back(&target);
}

void Z3DFilter::renderScreenQuad(const ZVertexArrayObject &vao, const Z3DShaderProgram &shader)
{
  if (!shader.isLinked())
    return;

  glDepthFunc(GL_ALWAYS);

  vao.bind();

  const GLfloat vertices[] = {-1.f, 1.f, 0.f, //top left corner
                              -1.f, -1.f, 0.f, //bottom left corner
                              1.f, 1.f, 0.f, //top right corner
                              1.f, -1.f, 0.f}; // bottom right rocner
  GLint attr_vertex = shader.vertexAttributeLocation();

  GLuint bufObjects[1];
  glGenBuffers(1, bufObjects);

  glEnableVertexAttribArray(attr_vertex);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
  glBufferData(GL_ARRAY_BUFFER, 3*4*sizeof(GLfloat), vertices, GL_STATIC_DRAW);
  glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, 0);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glDeleteBuffers(1, bufObjects);

  glDisableVertexAttribArray(attr_vertex);

  vao.release();

  glDepthFunc(GL_LESS);
}

void Z3DFilter::updateSize()
{
  // 1. update outport size
  glm::uvec2 maxOutportSize(0, 0);
  for(size_t i=0; i<m_outputPorts.size(); ++i) {
    glm::uvec2 outportSize = m_outputPorts[i]->expectedSize();
    if (outportSize.x > 0 && outportSize != m_outputPorts[i]->size()) {
      m_outputPorts[i]->resize(outportSize);
    }

    maxOutportSize = glm::max(maxOutportSize, m_outputPorts[i]->size());
  }

  // 2. update private ports
  for (size_t i=0; i<m_privateRenderPorts.size(); ++i) {
    m_privateRenderPorts[i]->resize(maxOutportSize);
  }
  for (size_t i=0; i<m_privateRenderTargets.size(); ++i) {
    m_privateRenderTargets[i]->resize(maxOutportSize);
  }

  // 3. update inport expected size
  for (size_t i=0; i<m_inputPorts.size(); i++) {
    m_inputPorts[i]->setExpectedSize(maxOutportSize);
  }

  invalidate();
}

} // namespace nim
