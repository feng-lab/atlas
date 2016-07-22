#include "z3dport.h"
#include <QsLog.h>

namespace nim {

Z3DPort::Z3DPort(const QString& name, bool allowMultipleConnections, Z3DFilter::InvalidationState invalidationState)
  : m_name(name)
  , m_filter(NULL)
  , m_allowMultipleConnections(allowMultipleConnections)
  , m_invalidationState(invalidationState)
{
}

Z3DPort::~Z3DPort()
{
}

void Z3DPort::setFilter(Z3DFilter *p)
{
  m_filter = p;
}

Z3DInputPortBase::Z3DInputPortBase(const QString &name, bool allowMultipleConnections, Z3DFilter::InvalidationState invalidationState)
  : Z3DPort(name, allowMultipleConnections, invalidationState)
  , m_expectedSize(0)
{
}

Z3DInputPortBase::~Z3DInputPortBase()
{
  disconnectAll();
}

void Z3DInputPortBase::invalidate()
{
  m_hasChanged = true;
  filter()->invalidate(m_invalidationState);
}

bool Z3DInputPortBase::isConnectedTo(const Z3DOutputPortBase *port) const
{
  return std::find(m_connectedOutputPorts.begin(), m_connectedOutputPorts.end(), port)
      != m_connectedOutputPorts.end();
}

bool Z3DInputPortBase::connect(Z3DOutputPortBase *outport)
{
  return outport->connect(this);
}

void Z3DInputPortBase::disconnect(Z3DOutputPortBase *outport)
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


Z3DOutputPortBase::Z3DOutputPortBase(const QString &name, bool allowMultipleConnections, Z3DFilter::InvalidationState invalidationState)
  : Z3DPort(name, allowMultipleConnections, invalidationState)
  , m_size(32,32)
{
}

Z3DOutputPortBase::~Z3DOutputPortBase()
{
  disconnectAll();
}

bool Z3DOutputPortBase::canConnectTo(const Z3DInputPortBase *inport) const
{
  if (!inport)
    return false;

  if (isConnectedTo(inport))
    return false;

  if ((inport->allowMultipleConnections() == false) && inport->isConnected())
    return false;

  if (filter() == inport->filter())
    return false;

  return true;
}

void Z3DOutputPortBase::invalidate()
{
  for (size_t i = 0; i < m_connectedInputPorts.size(); ++i)
    m_connectedInputPorts[i]->invalidate();
}

bool Z3DOutputPortBase::isConnectedTo(const Z3DInputPortBase *port) const
{
  return std::find(m_connectedInputPorts.begin(), m_connectedInputPorts.end(), port)
      != m_connectedInputPorts.end();
}

bool Z3DOutputPortBase::connect(Z3DInputPortBase *inport)
{
  if (canConnectTo(inport)) {
    m_connectedInputPorts.push_back(inport);
    inport->m_connectedOutputPorts.push_back(this);
    inport->invalidate();
    return true;
  }
  return false;
}

void Z3DOutputPortBase::disconnect(Z3DInputPortBase *inport)
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
  for (size_t j=0; j<m_connectedInputPorts.size(); ++j) {
    result = glm::max(result, m_connectedInputPorts[j]->expectedSize());
  }
  return result;
}

} // namespace nim





