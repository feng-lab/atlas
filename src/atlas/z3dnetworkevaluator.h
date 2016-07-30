#ifndef Z3DNETWORKEVALUATOR_H
#define Z3DNETWORKEVALUATOR_H

#include <QObject>
#include <vector>
#include "zbenchtimer.h"
#include "z3dcanvas.h"

#ifndef Q_MOC_RUN

#include <boost/graph/adjacency_list.hpp>

#endif

namespace nim {

class Z3DFilter;

class Z3DCanvasPainter;

class Z3DOutputPortBase;

class Z3DInputPortBase;

class Z3DFilterWrapper
{
public:
  virtual ~Z3DFilterWrapper()
  {}

  virtual void beforeFilterProcess(const Z3DFilter*)
  {}

  virtual void afterFilterProcess(const Z3DFilter*)
  {}

  virtual void beforeNetworkProcess()
  {}

  virtual void afterNetworkProcess()
  {}
};

class Z3DNetworkEvaluator : public QObject
{
Q_OBJECT

public:
  Z3DNetworkEvaluator(QObject* parent = 0);

  ~Z3DNetworkEvaluator();

  void setOpenGLContext(Z3DCanvas* context)
  { m_openGLContext = context; }

  // set canvasPainter as the sink of rendering network and build network
  void setNetworkSink(Z3DCanvasPainter* canvasPainter);

  // process the currently assigned network. The rendering order is determined internally
  // according the network topology and the invalidation levels of the filters.
  // stereo means run two passes for left and right eye
  void process(bool stereo = false);

  void initializeNetwork();

  // call when network topology changed
  void updateNetwork();

protected:
  void buildNetwork();

  // Locks the evaluator. In this state, it does not perform
  // any operations, such as initializing or processing, on the filter network
  void lock()
  { m_locked = true; }

  void unlock()
  { m_locked = false; }

  inline void getGLFocus() const
  { if (m_openGLContext) m_openGLContext->getGLFocus(); }

  // update size of all upstream filters. If input filter is nullptr, update all filters
  void sizeChangedFromFilter(Z3DFilter* rp = 0);

private:
  std::vector<Z3DFilter*> m_renderingOrder;

  std::vector<std::unique_ptr<Z3DFilterWrapper>> m_filterWrappers;

  // used to make sure we operate in the correct opengl context
  Z3DCanvas* m_openGLContext;

  bool m_locked;

  bool m_processPending;

  Z3DCanvasPainter* m_canvasPainter;

  struct VertexInfo
  {
    VertexInfo() : filter(0)
    {}

    VertexInfo(Z3DFilter* p) : filter(p)
    {}

    Z3DFilter* filter;
    //
  };

  struct EdgeInfo
  {
    EdgeInfo(Z3DOutputPortBase* out, Z3DInputPortBase* in) : outPort(out), inPort(in)
    {}

    Z3DOutputPortBase* outPort;
    Z3DInputPortBase* inPort;
  };

  typedef boost::adjacency_list<boost::listS, boost::vecS, boost::bidirectionalS, VertexInfo, EdgeInfo> GraphT;
  typedef boost::graph_traits<GraphT>::vertex_descriptor Vertex;
  typedef boost::graph_traits<GraphT>::edge_descriptor Edge;
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
  void checkState(const Z3DFilter* p = 0);

  void warn(const Z3DFilter* p, const QString& message);
};

// profile each filter and whole network
class Z3DProfileFilterWrapper : public Z3DFilterWrapper
{
  ZBenchTimer m_benchTimer;
public:
  void beforeFilterProcess(const Z3DFilter*) override;

  void afterFilterProcess(const Z3DFilter* p) override;

  void beforeNetworkProcess() override;

  void afterNetworkProcess() override;
};

} // namespace nim

#endif // Z3DNETWORKEVALUATOR_H
