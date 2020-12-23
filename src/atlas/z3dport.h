#pragma once

#include "z3dfilter.h"
#include "zlog.h"
#include <vector>

namespace nim {

class Z3DOutputPortBase;

class Z3DInputPortBase
{
public:
  Z3DInputPortBase(QString name, bool allowMultipleConnections,
                   Z3DFilter* filter,
                   Z3DFilter::State invalidationState = Z3DFilter::State::AllResultInvalid);

  virtual ~Z3DInputPortBase();

  // return the filter this port belongs to.
  [[nodiscard]] Z3DFilter* filter() const
  { return m_filter; }

  [[nodiscard]] QString name() const
  { return m_name; }

  // invalidate filter with the given State and set hasChanged=true.
  void invalidate();

  // has the data in this port changed since the last process() call?
  [[nodiscard]] bool hasChanged() const
  { return m_hasChanged; }

  // mark the port as valid.
  void setValid()
  { m_hasChanged = false; }

  [[nodiscard]] const std::vector<Z3DOutputPortBase*> connected() const
  { return m_connectedOutputPorts; }

  [[nodiscard]] size_t numConnections() const
  { return m_connectedOutputPorts.size(); }

  [[nodiscard]] bool isConnected() const
  { return !m_connectedOutputPorts.empty(); }

  bool isConnectedTo(const Z3DOutputPortBase* port) const;

  // return true if the port is connected and contains valid data.
  [[nodiscard]] virtual bool isReady() const = 0;

  bool connect(Z3DOutputPortBase* outport);

  void disconnect(Z3DOutputPortBase* outport);

  void disconnectAll();

  void setExpectedSize(const glm::uvec2& size)
  { m_expectedSize = size; }

  [[nodiscard]] glm::uvec2 expectedSize() const
  { return m_expectedSize; }

protected:
  friend class Z3DOutputPortBase;

  QString m_name;
  bool m_allowMultipleConnections;
  Z3DFilter* m_filter = nullptr;

  // how changes from this port affect its filter
  Z3DFilter::State m_invalidationState;

  std::vector<Z3DOutputPortBase*> m_connectedOutputPorts;
  bool m_hasChanged = true;

  glm::uvec2 m_expectedSize;
};

class Z3DOutputPortBase
{
public:
  Z3DOutputPortBase(QString name, Z3DFilter* filter);

  virtual ~Z3DOutputPortBase();

  // return the filter this port belongs to.
  [[nodiscard]] Z3DFilter* filter() const
  { return m_filter; }

  [[nodiscard]] QString name() const
  { return m_name; }

  [[nodiscard]] const std::vector<Z3DInputPortBase*> connected() const
  { return m_connectedInputPorts; }

  // test if this outport can connect to a given inport.
  virtual bool canConnectTo(const Z3DInputPortBase* inport) const;

  // invalidate all connected inports.
  virtual void invalidate();

  [[nodiscard]] size_t numConnections() const
  { return m_connectedInputPorts.size(); }

  [[nodiscard]] bool isConnected() const
  { return !m_connectedInputPorts.empty(); }

  bool isConnectedTo(const Z3DInputPortBase* port) const;

  // returns whether the port is ready to be used by its owning filter.
  // return true if the port is connected.
  [[nodiscard]] virtual bool isReady() const
  { return isConnected(); }

  // return true if this output port contains valid data
  [[nodiscard]] virtual bool hasValidData() const = 0;

  bool connect(Z3DInputPortBase* inport);

  void disconnect(Z3DInputPortBase* inport);

  void disconnectAll();

  [[nodiscard]] glm::uvec2 size() const
  { return m_size; }

  // return the maximum of expectesize of all connected inports.
  // If no inport connected, return (0, 0)
  [[nodiscard]] glm::uvec2 expectedSize() const;

  virtual void resize(const glm::uvec2& newsize)
  { m_size = newsize; }

protected:
  QString m_name;
  Z3DFilter* m_filter = nullptr;

  std::vector<Z3DInputPortBase*> m_connectedInputPorts;

  glm::uvec2 m_size;
};

template<typename T>
class Z3DFilterInputPort : public Z3DInputPortBase
{
public:
  Z3DFilterInputPort(const QString& name, bool allowMultipleConnections,
                     Z3DFilter* filter,
                     Z3DFilter::State invalidationState = Z3DFilter::State::AllResultInvalid)
    : Z3DInputPortBase(name, allowMultipleConnections, filter, invalidationState)
  {}

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
    if (isConnected()) {
      return static_cast<T*>(m_connectedOutputPorts[0]->filter());
    } else {
      return 0;
    }
  }

  [[nodiscard]] bool isReady() const override
  { return isConnected(); }
};

template<typename T>
class Z3DFilterOutputPort : public Z3DOutputPortBase
{
public:
  Z3DFilterOutputPort(const QString& name, Z3DFilter* filter)
    : Z3DOutputPortBase(name, filter)
  {}

  bool canConnectTo(const Z3DInputPortBase* inport) const override
  {
    if (dynamic_cast<const Z3DFilterInputPort<T>*>(inport)) {
      return Z3DOutputPortBase::canConnectTo(inport);
    } else {
      return false;
    }
  }

  // data is filter itself, so it is always valid
  [[nodiscard]] bool hasValidData() const override
  { return true; }
};

} // namespace nim


