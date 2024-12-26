#include "z3dfilter.h"

#include "zlog.h"
#include "zsysteminfo.h"
#include "z3dport.h"
#include "z3dinteractionhandler.h"
#include "z3dshaderprogram.h"
#include "zeventlistenerparameter.h"
#include "zparameter.h"
#include "z3dvertexarrayobject.h"

namespace nim {

Z3DFilter::Z3DFilter(QObject* parent)
  : QObject(parent)
  , m_state(State::AllResultInvalid)
  , m_invalidationVisited(false)
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
  CHECK(inv != State::Valid);
  if (isFlagSet(m_state, inv)) {
    return;
  }

  setFlag(m_state, inv);

  if (!m_invalidationVisited) {
    m_invalidationVisited = true;

    for (auto port : m_outputPorts) {
      port->invalidate();
    }

    m_invalidationVisited = false;
  }
}

Z3DInputPortBase* Z3DFilter::inputPort(const QString& name) const
{
  for (auto port : m_inputPorts) {
    if (port->name() == name) {
      return port;
    }
  }

  return nullptr;
}

Z3DOutputPortBase* Z3DFilter::outputPort(const QString& name) const
{
  for (auto port : m_outputPorts) {
    if (port->name() == name) {
      return port;
    }
  }

  return nullptr;
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

void Z3DFilter::disconnectAllPorts()
{
  for (auto port : m_inputPorts) {
    port->disconnectAll();
  }

  for (auto port : m_outputPorts) {
    port->disconnectAll();
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
  bool isReady = std::ranges::all_of(m_inputPorts, [](auto port) {
    return port->isReady();
  });
  if (isReady) {
    isReady = std::ranges::all_of(m_outputPorts, [](auto port) {
      return port->isReady();
    });
  }
  return isReady;
}

void Z3DFilter::addPort(Z3DInputPortBase& port)
{
  if (m_inputPortMap.count(port.name())) {
    LOG(FATAL) << className() << " port " << port.name() << " has already been inserted!";
  } else {
    m_inputPortMap.emplace(port.name(), &port);
    m_inputPorts.push_back(&port);
  }
}

void Z3DFilter::addPort(Z3DOutputPortBase& port)
{
  if (m_outputPortMap.count(port.name())) {
    LOG(FATAL) << className() << " port " << port.name() << " has already been inserted!";
  } else {
    m_outputPortMap.emplace(port.name(), &port);
    m_outputPorts.push_back(&port);
  }
}

void Z3DFilter::removePort(Z3DInputPortBase& port)
{
  std::erase(m_inputPorts, &port);

  if (m_inputPortMap.erase(port.name()) == 0) {
    LOG(FATAL) << className() << " port " << port.name() << " was not found!";
  }
}

void Z3DFilter::removePort(Z3DOutputPortBase& port)
{
  std::erase(m_outputPorts, &port);

  if (m_outputPortMap.erase(port.name()) == 0) {
    LOG(FATAL) << className() << " port " << port.name() << " was not found!";
  }
}

void Z3DFilter::addParameter(ZParameter& para, State inv)
{
  if (m_parameterNames.count(para.name())) {
    LOG(FATAL) << "Duplicated para name " << para.name();
  }
  m_parameters.push_back(&para);
  m_parameterNames.insert(para.name());
  if (inv != State::Valid) {
    connect(&para, &ZParameter::valueChanged, this, &Z3DFilter::invalidateResult);
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

void Z3DFilter::renderScreenQuad(const Z3DVertexArrayObject& vao, const Z3DShaderProgram& shader)
{
  if (!shader.isLinked()) {
    return;
  }

  glDepthFunc(GL_ALWAYS);

  vao.bind();

  const GLfloat vertices[] = {-1.f,
                              1.f,
                              0.f, // top left corner
                              -1.f,
                              -1.f,
                              0.f, // bottom left corner
                              1.f,
                              1.f,
                              0.f, // top right corner
                              1.f,
                              -1.f,
                              0.f}; // bottom right rocner
  auto attr_vertex = shader.vertexAttributeLocation();

  GLuint bufObjects[1];
  glGenBuffers(1, bufObjects);

  glEnableVertexAttribArray(attr_vertex);
  glBindBuffer(GL_ARRAY_BUFFER, bufObjects[0]);
  glBufferData(GL_ARRAY_BUFFER, 3 * 4 * sizeof(GLfloat), vertices, GL_STATIC_DRAW);
  glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

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
  for (auto port : m_outputPorts) {
    // VLOG(1) << className() << " " << port->expectedSize() << " " << port->size() << " " << port->name();
    glm::uvec2 outportSize = port->expectedSize();
    if (outportSize.x > 0 && outportSize != port->size()) {
      port->resize(outportSize);
      // VLOG(1) << className() << " " << outportSize;
    }

    maxOutportSize = glm::max(maxOutportSize, port->size());
  }

  // 2. update private ports
  //  for (auto port : m_privateRenderPorts) {
  //    port->resize(maxOutportSize);
  //  }
  //  for (auto target : m_privateRenderTargets) {
  //    target->resize(maxOutportSize);
  //  }

  // 2. update inport expected size
  for (auto port : m_inputPorts) {
    port->setExpectedSize(maxOutportSize);
  }

  invalidate(State::AllResultInvalid);
}

} // namespace nim
