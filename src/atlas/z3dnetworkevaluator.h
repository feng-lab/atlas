#pragma once

#include "zbenchtimer.h"
#include <QObject>
#ifndef Q_MOC_RUN
#include <folly/CancellationToken.h>
#include <boost/graph/adjacency_list.hpp>
#endif
#include <vector>

namespace nim {

class Z3DFilter;

class Z3DCompositor;

class Z3DOutputPortBase;

class Z3DInputPortBase;

class Z3DFilterWrapper
{
public:
  virtual ~Z3DFilterWrapper() = default;

  virtual void beforeFilterProcess(const Z3DFilter*) {}

  virtual void afterFilterProcess(const Z3DFilter*) {}

  virtual void beforeNetworkProcess() {}

  virtual void afterNetworkProcess() {}
};

class Z3DNetworkEvaluator : public QObject
{
  Q_OBJECT

public:
  explicit Z3DNetworkEvaluator(Z3DCompositor& compositor, QObject* parent = nullptr);

  // process the currently assigned network. The rendering order is determined internally
  // according the network topology and the invalidation levels of the filters.
  // stereo means run two passes for left and right eye
  double process(bool stereo = false,
                 bool progressiveRendering = false,
                 const folly::CancellationToken& cancellationToken = folly::CancellationToken());

  // call when network topology changed
  void updateNetwork();

protected:
  // update size of all upstream filters. If input filter is nullptr, update all filters
  void sizeChangedFromFilter(Z3DFilter* rp = nullptr);

private:
  std::vector<Z3DFilter*> m_renderingOrder;

  std::vector<std::unique_ptr<Z3DFilterWrapper>> m_filterWrappers;

  Z3DCompositor& m_compositor;

  struct VertexInfo
  {
    VertexInfo()
      : filter(nullptr)
    {}

    explicit VertexInfo(Z3DFilter* p)
      : filter(p)
    {}

    Z3DFilter* filter;
    //
  };

  struct EdgeInfo
  {
    EdgeInfo(Z3DOutputPortBase* out, Z3DInputPortBase* in)
      : outPort(out)
      , inPort(in)
    {}

    Z3DOutputPortBase* outPort;
    Z3DInputPortBase* inPort;
  };

  using GraphT = boost::adjacency_list<boost::listS, boost::vecS, boost::bidirectionalS, VertexInfo, EdgeInfo>;
  using Vertex = boost::graph_traits<GraphT>::vertex_descriptor;
  using Edge = boost::graph_traits<GraphT>::edge_descriptor;
  std::map<Z3DFilter*, Vertex> m_filterToVertexMapper;
  GraphT m_filterGraph;

  std::vector<Z3DFilter*> m_reverseSortedFilters;
};

// check if OpenGL state conforms to default settings. Log a warning message if not.
class Z3DCheckOpenGLStateFilterWrapper : public Z3DFilterWrapper
{
public:
  void afterFilterProcess(const Z3DFilter* p) override;

  void beforeNetworkProcess() override;

private:
  void checkState(const Z3DFilter* p = nullptr);

  static void warn(const Z3DFilter* p, const char* message);
};

// profile each filter and whole network
class Z3DProfileFilterWrapper : public Z3DFilterWrapper
{
  ZBenchTimer m_benchTimer{"Network"};

public:
  void afterFilterProcess(const Z3DFilter* p) override;

  void beforeNetworkProcess() override;

  void afterNetworkProcess() override;
};

} // namespace nim
