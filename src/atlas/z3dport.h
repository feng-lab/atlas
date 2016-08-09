#pragma once

#include "zlog.h"
#include "z3dfilter.h"

#include <vector>

namespace nim {

class Z3DPort
{
public:
  Z3DPort(const QString& name, bool allowMultipleConnections = false,
          Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult);

  virtual ~Z3DPort();

  bool allowMultipleConnections() const
  { return m_allowMultipleConnections; }

  // return the filter this port belongs to.
  Z3DFilter* filter() const
  { return m_filter; }

  QString name() const
  { return m_name; }

  virtual void setFilter(Z3DFilter* p);

protected:
  QString m_name;
  Z3DFilter* m_filter;
  bool m_allowMultipleConnections;

  // how changes from this port affect its filter
  Z3DFilter::InvalidationState m_invalidationState;
};

class Z3DOutputPortBase;

class Z3DInputPortBase : public Z3DPort
{
public:
  Z3DInputPortBase(const QString& name, bool allowMultipleConnections = false,
                   Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult);

  virtual ~Z3DInputPortBase();

  // invalidate filter with the given InvalidationState and set hasChanged=true.
  void invalidate();

  // has the data in this port changed since the last process() call?
  bool hasChanged() const
  { return m_hasChanged; }

  // mark the port as valid.
  void setValid()
  { m_hasChanged = false; }

  const std::vector<Z3DOutputPortBase*> connected() const
  { return m_connectedOutputPorts; }

  size_t numConnections() const
  { return m_connectedOutputPorts.size(); }

  bool isConnected() const
  { return !m_connectedOutputPorts.empty(); }

  bool isConnectedTo(const Z3DOutputPortBase* port) const;

  // return true if the port is connected and contains valid data.
  virtual bool isReady() const = 0;

  bool connect(Z3DOutputPortBase* outport);

  void disconnect(Z3DOutputPortBase* outport);

  void disconnectAll();

  void setExpectedSize(glm::uvec2 size)
  { m_expectedSize = size; }

  glm::uvec2 expectedSize() const
  { return m_expectedSize; }

protected:
  friend class Z3DOutputPortBase;

  std::vector<Z3DOutputPortBase*> m_connectedOutputPorts;
  bool m_hasChanged;

  glm::uvec2 m_expectedSize;
};

class Z3DOutputPortBase : public Z3DPort
{
public:
  Z3DOutputPortBase(const QString& name, bool allowMultipleConnections = true,
                    Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult);

  virtual ~Z3DOutputPortBase();

  const std::vector<Z3DInputPortBase*> connected() const
  { return m_connectedInputPorts; }

  // test if this outport can connect to a given inport.
  virtual bool canConnectTo(const Z3DInputPortBase* inport) const;

  // invalidate all connected inports.
  virtual void invalidate();

  size_t numConnections() const
  { return m_connectedInputPorts.size(); }

  bool isConnected() const
  { return !m_connectedInputPorts.empty(); }

  bool isConnectedTo(const Z3DInputPortBase* port) const;

  // returns whether the port is ready to be used by its owning filter.
  // return true if the port is connected.
  virtual bool isReady() const
  { return isConnected(); }

  // return true if this output port contains valid data
  virtual bool hasValidData() const = 0;

  bool connect(Z3DInputPortBase* inport);

  void disconnect(Z3DInputPortBase* inport);

  void disconnectAll();

  glm::uvec2 size() const
  { return m_size; }

  // return the maximum of expectesize of all connected inports.
  // If no inport connected, return (0, 0)
  glm::uvec2 expectedSize() const;

  virtual void resize(const glm::uvec2& newsize)
  { m_size = newsize; }

protected:
  std::vector<Z3DInputPortBase*> m_connectedInputPorts;

  glm::uvec2 m_size;
};

template<class T>
class Z3DInputPort;

template<typename T>
class Z3DOutputPort : public Z3DOutputPortBase
{
public:
  Z3DOutputPort(const QString& name, bool allowMultipleConnections = true,
                Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult)
    : Z3DOutputPortBase(name, allowMultipleConnections, invalidationState)
    , m_portData(0)
  {}

  virtual ~Z3DOutputPort()
  {
  }

  virtual bool canConnectTo(const Z3DInputPortBase* inport) const override
  {
    if (dynamic_cast<const Z3DInputPort<T>*>(inport))
      return Z3DOutputPortBase::canConnectTo(inport);
    else
      return false;
  }

  void setData(T* data)
  {
    // is it possible that the new allocated data has the same address as the old one???
    if (data != m_portData) {
      m_portData = data;
      invalidate();
    }
  }

  // return the data stored in this port
  T* data() const
  {
    return m_portData;
  }

  virtual bool hasValidData() const override
  { return m_portData != nullptr; }

  std::vector<const Z3DInputPort<T>*> connected() const
  {
    std::vector<const Z3DInputPort<T>*> ports;
    for (size_t i = 0; i < m_connectedInputPorts.size(); ++i) {
      Z3DInputPort<T>* p = static_cast<Z3DInputPort<T>*>(m_connectedInputPorts[i]);
      ports.push_back(p);
    }
    return ports;
  }

protected:
  T* m_portData;
};

template<typename T>
class Z3DInputPort : public Z3DInputPortBase
{
public:
  Z3DInputPort(const QString& name, bool allowMultipleConnections = true,
               Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult)
    : Z3DInputPortBase(name, allowMultipleConnections, invalidationState)
  {}

  virtual ~Z3DInputPort()
  {}


  // return first valid data stored in the connected outports
  T* firstValidData() const
  {
    for (size_t i = 0; i < m_connectedOutputPorts.size(); ++i) {
      Z3DOutputPort<T>* p = static_cast< Z3DOutputPort<T>* >(m_connectedOutputPorts[i]);
      if (p->hasValidData())
        return p->data();
    }
    return nullptr;
  }

  // return all valid data stored in the connected outports
  std::vector<T*> allValidData() const
  {
    std::vector<T*> allData;

    for (size_t i = 0; i < m_connectedOutputPorts.size(); ++i) {
      Z3DOutputPort<T>* p = static_cast<Z3DOutputPort<T>*>(m_connectedOutputPorts[i]);
      if (p->hasValidData())
        allData.push_back(p->data());
    }

    return allData;
  }

  std::vector<const Z3DOutputPort<T>*> connected() const
  {
    std::vector<const Z3DOutputPort<T>*> ports;
    for (size_t i = 0; i < m_connectedOutputPorts.size(); ++i) {
      Z3DOutputPort<T>* p = static_cast<Z3DOutputPort<T>*>(m_connectedOutputPorts[i]);
      ports.push_back(p);
    }
    return ports;
  }

  virtual bool isReady() const override
  { return firstValidData() != nullptr; }
};


template<typename T>
class Z3DFilterInputPort : public Z3DInputPortBase
{
public:
  Z3DFilterInputPort(const QString& name, bool allowMultipleConnections = true,
                     Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult)
    : Z3DInputPortBase(name, allowMultipleConnections, invalidationState)
  {
  }

  std::vector<T*> connectedFilters() const
  {
    std::vector<T*> filters;
    for (size_t i = 0; i < m_connectedOutputPorts.size(); ++i) {
      T* p = static_cast<T*>(m_connectedOutputPorts[i]->filter());
      filters.push_back(p);
    }
    return filters;
  }

  T* firstConnectedFilter() const
  {
    if (isConnected())
      return static_cast<T*>(m_connectedOutputPorts[0]->filter());
    else
      return 0;
  }

  virtual bool isReady() const override
  { return isConnected(); }
};

template<typename T>
class Z3DFilterOutputPort : public Z3DOutputPortBase
{
public:
  Z3DFilterOutputPort(const QString& name, bool allowMultipleConnections = false,
                      Z3DFilter::InvalidationState invalidationState = Z3DFilter::InvalidAllResult)
    : Z3DOutputPortBase(name, allowMultipleConnections, invalidationState)
  {
  }

  virtual bool canConnectTo(const Z3DInputPortBase* inport) const override
  {
    if (dynamic_cast<const Z3DFilterInputPort<T>*>(inport))
      return Z3DOutputPortBase::canConnectTo(inport);
    else
      return false;
  }

  // data is filter itself, so it is always valid
  virtual bool hasValidData() const override
  { return true; }

protected:
  virtual void setFilter(Z3DFilter* p) override
  {
    Z3DOutputPortBase::setFilter(p);
    T* tp = dynamic_cast<T*>(p);
    if (!tp) {
      LOG(ERROR) << "Port " << name() << " attached to filter of wrong type " << p->className();
    }
  }
};

} // namespace nim


