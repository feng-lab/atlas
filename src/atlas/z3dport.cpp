#include "z3dport.h"
#include "z3dfilter.h"

#include <utility>

#include "zlog.h"

namespace nim {

Z3DInputPortBase::Z3DInputPortBase(QString name,
                                   bool allowMultipleConnections,
                                   Z3DFilter* filter,
                                   Z3DFilter::State invalidationState)
  : m_name(std::move(name))
  , m_allowMultipleConnections(allowMultipleConnections)
  , m_filter(filter)
  , m_invalidationState(invalidationState)
  , m_expectedSize(0)
{
  CHECK(filter);
}

Z3DInputPortBase::~Z3DInputPortBase()
{
  disconnectAll();
}

void Z3DInputPortBase::invalidate()
{
  // Tag downstream filter with the specific inport cause and state.
#ifdef NO // ATLAS_DEBUG_VERSION
  if (m_filter) {
    m_filter->debugSetInvalidateReason(
      QString("inport '%1' invalidated (state=%2)").arg(m_name, nim::enumToQString(m_invalidationState)));
  }
#endif
  filter()->invalidate(m_invalidationState);
}

bool Z3DInputPortBase::isConnectedTo(const Z3DOutputPortBase* port) const
{
  return contains(m_connectedOutputPorts, port);
}

bool Z3DInputPortBase::connect(Z3DOutputPortBase* outport)
{
  return outport->connect(this);
}

void Z3DInputPortBase::disconnect(Z3DOutputPortBase* outport)
{
  for (size_t i = 0; i < m_connectedOutputPorts.size(); ++i) {
    if (m_connectedOutputPorts[i] == outport) {
      m_connectedOutputPorts.erase(m_connectedOutputPorts.begin() + i);
      outport->disconnect(this);
      return;
    }
  }
}

void Z3DInputPortBase::disconnectAll()
{
  while (!m_connectedOutputPorts.empty()) {
    m_connectedOutputPorts[0]->disconnect(this);
  }
}

Z3DOutputPortBase::Z3DOutputPortBase(QString name, Z3DFilter* filter)
  : m_name(std::move(name))
  , m_filter(filter)
  , m_size(32, 32)
{
  CHECK(filter);
}

Z3DOutputPortBase::~Z3DOutputPortBase()
{
  disconnectAll();
}

bool Z3DOutputPortBase::canConnectTo(const Z3DInputPortBase* inport) const
{
  if (!inport) {
    return false;
  }

  if (isConnectedTo(inport)) {
    return false;
  }

  if (!inport->m_allowMultipleConnections && inport->isConnected()) {
    return false;
  }

  if (filter() == inport->filter()) {
    return false;
  }

  return true;
}

void Z3DOutputPortBase::invalidate()
{
  for (size_t i = 0; i < m_connectedInputPorts.size(); ++i) {
    // Attach a reason to the downstream filter for debugging attribution.
#ifdef NO  // ATLAS_DEBUG_VERSION
    if (auto* f = m_connectedInputPorts[i]->filter()) {
      f->debugSetInvalidateReason(
        QString("upstream outport '%1' of '%2' invalidated").arg(m_name, m_filter->className()));
    }
#endif
    m_connectedInputPorts[i]->invalidate();
  }
}

bool Z3DOutputPortBase::isConnectedTo(const Z3DInputPortBase* port) const
{
  return contains(m_connectedInputPorts, port);
}

bool Z3DOutputPortBase::connect(Z3DInputPortBase* inport)
{
  if (canConnectTo(inport)) {
    m_connectedInputPorts.push_back(inport);
    inport->m_connectedOutputPorts.push_back(this);
    inport->invalidate();
    return true;
  }
  LOG(ERROR) << "Inport " << inport->name() << " of " << inport->filter()->className()
             << " can not be connected to outport " << m_name << " of " << m_filter->className();
  return false;
}

void Z3DOutputPortBase::disconnect(Z3DInputPortBase* inport)
{
  for (size_t i = 0; i < m_connectedInputPorts.size(); ++i) {
    if (m_connectedInputPorts[i] == inport) {
      m_connectedInputPorts.erase(m_connectedInputPorts.begin() + i);
      inport->disconnect(this);
      invalidate();
      return;
    }
  }
}

void Z3DOutputPortBase::disconnectAll()
{
  while (!m_connectedInputPorts.empty()) {
    m_connectedInputPorts[0]->disconnect(this);
  }
}

glm::uvec2 Z3DOutputPortBase::expectedSize() const
{
  glm::uvec2 result(0, 0);
  for (size_t j = 0; j < m_connectedInputPorts.size(); ++j) {
    result = glm::max(result, m_connectedInputPorts[j]->expectedSize());
  }
  return result;
}

} // namespace nim
