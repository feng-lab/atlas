#include "z3dgl.h"
#include "z3dnetworkevaluator.h"
#include <set>
#include <queue>
#include <algorithm>
#include <boost/graph/topological_sort.hpp>
#include "z3dcanvaspainter.h"
#include "z3dfilter.h"
#include "zlog.h"
#include "z3dtexture.h"
#include "z3drendertarget.h"
#include "z3dmeshfilter.h"
#include "zrandom.h"

//#define PROFILE3DRENDERERS

namespace nim {

Z3DNetworkEvaluator::Z3DNetworkEvaluator(QObject *parent)
  : QObject(parent)
  , m_openGLContext(NULL)
  , m_locked(false)
  , m_processPending(false)
  , m_canvasPainter(NULL)
{
#if defined(_DEBUG_)
  m_filterWrappers.emplace_back(std::make_unique<Z3DCheckOpenGLStateFilterWrapper>());
#endif
#if defined(PROFILE3DRENDERERS)
  m_filterWrappers.emplace_back(std::make_unique<Z3DProfileFilterWrapper>());
#endif
}

Z3DNetworkEvaluator::~Z3DNetworkEvaluator()
{
}

void Z3DNetworkEvaluator::setNetworkSink(Z3DCanvasPainter *canvasPainter)
{
  if (m_canvasPainter == canvasPainter)
    return;

  m_canvasPainter = canvasPainter;

  buildNetwork();
}

void Z3DNetworkEvaluator::process(bool stereo)
{
  if (!m_canvasPainter)
    return;

  if (m_locked) {
    LOG(INFO) << "locked. Scheduling.";
    //m_processPending = true;
    return;
  }

  lock();

//  for (size_t i = 0; i < m_renderingOrder.size(); ++i) {
//    Z3DMeshFilter* meshFilter = dynamic_cast<Z3DMeshFilter*>(m_renderingOrder[i]);
//    if (meshFilter && !meshFilter->isFixed()) {
//      if (ZRandomInstance.randReal<float>() < 0.001f) {
//        meshFilter->setVisible(true);
//        meshFilter->setGlow(true);
//        meshFilter->setStayOnTop(true);
//      } else {
//        meshFilter->setVisible(false);
//      }
//    }
//  }

  // notify filter wrappers
  for (size_t j = 0; j < m_filterWrappers.size(); ++j)
    m_filterWrappers[j]->beforeNetworkProcess();
  CHECK_GL_ERROR;

  // Iterate over filters in rendering order
  for (size_t i = 0; i < m_renderingOrder.size(); ++i) {
    Z3DFilter* currentFilter = m_renderingOrder[i];

    Z3DEye eye = stereo ? Z3DEye::Left : Z3DEye::Mono;

    // execute the filter, if it needs processing and is ready
    if (!currentFilter->isValid(eye) && currentFilter->isReady(eye)) {
      // notify filter wrappers
      for (size_t j=0; j < m_filterWrappers.size(); ++j)
        m_filterWrappers[j]->beforeFilterProcess(currentFilter);
      CHECK_GL_ERROR;

      {
        getGLFocus();
        currentFilter->process(eye);
        currentFilter->setValid(eye);
        CHECK_GL_ERROR;
      }

      // notify filter wrappers
      getGLFocus();
      for (size_t j = 0; j < m_filterWrappers.size(); ++j)
        m_filterWrappers[j]->afterFilterProcess(currentFilter);
      CHECK_GL_ERROR;
    }

    if (stereo && !currentFilter->isValid(Z3DEye::Right) && currentFilter->isReady(Z3DEye::Right)) {
      // notify filter wrappers
      for (size_t j=0; j < m_filterWrappers.size(); ++j)
        m_filterWrappers[j]->beforeFilterProcess(currentFilter);
      CHECK_GL_ERROR;

      {
        getGLFocus();
        currentFilter->process(Z3DEye::Right);
        currentFilter->setValid(Z3DEye::Right);
        CHECK_GL_ERROR;
      }

      // notify filter wrappers
      getGLFocus();
      for (size_t j = 0; j < m_filterWrappers.size(); ++j)
        m_filterWrappers[j]->afterFilterProcess(currentFilter);
      CHECK_GL_ERROR;
    }
  }

  // notify filter wrappers
  for (size_t j = 0; j < m_filterWrappers.size(); ++j)
    m_filterWrappers[j]->afterNetworkProcess();
  CHECK_GL_ERROR;

  unlock();

  // make sure that canvases are repainted, if their update has been blocked by the locked evaluator
  if (m_processPending) {
    m_processPending = false;
    m_canvasPainter->invalidate();
  }
}

void Z3DNetworkEvaluator::initializeNetwork()
{
  if (m_locked) {
    LOG(INFO) << "locked.";
  }

  lock();

  // update size
  sizeChangedFromFilter();
  for (size_t i=0; i<m_reverseSortedFilters.size(); i++) {
    QObject::disconnect(m_reverseSortedFilters[i], &Z3DFilter::requestUpstreamSizeChange, 0, 0);
    connect(m_reverseSortedFilters[i], &Z3DFilter::requestUpstreamSizeChange,
            this, &Z3DNetworkEvaluator::sizeChangedFromFilter);
  }

  unlock();
  CHECK_GL_ERROR;
}

void Z3DNetworkEvaluator::updateNetwork()
{
  buildNetwork();
  initializeNetwork();
}

void Z3DNetworkEvaluator::buildNetwork()
{
  m_renderingOrder.clear();
  m_filterToVertexMapper.clear();
  m_filterGraph.clear();
  m_reverseSortedFilters.clear();

  // nothing more to do, if no network sink is present
  if (!m_canvasPainter)
    return;

  std::queue<Z3DFilter*> filterQueue;

  filterQueue.push(m_canvasPainter);
  Vertex v = boost::add_vertex(VertexInfo(m_canvasPainter), m_filterGraph);
  m_filterToVertexMapper[m_canvasPainter] = v;

  // build graph of all connected filters
  while (!filterQueue.empty()) {
    Z3DFilter *filter = filterQueue.front();
    const std::vector<Z3DInputPortBase*> inports = filter->inputPorts();
    for (size_t i = 0; i < inports.size(); ++i) {
      const std::vector<Z3DOutputPortBase*> connected = inports[i]->connected();
      for (size_t j = 0; j < connected.size(); ++j) {
        Z3DFilter *outFilter = connected[j]->filter();
        if (m_filterToVertexMapper.find(outFilter) == m_filterToVertexMapper.end()) {
          filterQueue.push(outFilter);
          Vertex v = boost::add_vertex(VertexInfo(outFilter), m_filterGraph);
          m_filterToVertexMapper[outFilter] = v;
        }
        boost::add_edge(m_filterToVertexMapper[outFilter],
                        m_filterToVertexMapper[filter],
                        EdgeInfo(connected[j], inports[i]),
                        m_filterGraph);
      }
    }

    filterQueue.pop();
  }

  // sort to get rendering order
  std::vector<Vertex> sorted;
  boost::topological_sort(m_filterGraph, std::back_inserter(sorted));
  for (std::vector<Vertex>::reverse_iterator rit = sorted.rbegin();
       rit != sorted.rend(); rit++) {
    m_renderingOrder.push_back(m_filterGraph[*rit].filter);
  }

  LOG(INFO) << "Rendering Order: ";
  for (size_t i=0; i<m_renderingOrder.size(); i++) {
    LOG(INFO) << "  " << i << ": " << m_renderingOrder[i]->className();
  }
  LOG(INFO) << "";

  // update reverse sorted filters
  m_reverseSortedFilters = m_renderingOrder;
  std::reverse(m_reverseSortedFilters.begin(), m_reverseSortedFilters.end());
}

void Z3DNetworkEvaluator::sizeChangedFromFilter(Z3DFilter *rp)
{
  if (rp) {
    bool started = false;
    for (size_t i=0; i<m_reverseSortedFilters.size(); i++) {
      if (started)
        m_reverseSortedFilters[i]->updateSize();
      else {
        if (rp == m_reverseSortedFilters[i])
          started = true;
      }
    }
  } else {
    for (size_t i=0; i<m_reverseSortedFilters.size(); i++) {
      m_reverseSortedFilters[i]->updateSize();
    }
  }
}

// ----------------------------------------------------------------------------

void Z3DCheckOpenGLStateFilterWrapper::afterFilterProcess(const Z3DFilter* p)
{
  checkState(p);
}

void Z3DCheckOpenGLStateFilterWrapper::beforeNetworkProcess()
{
  checkState();
}

void Z3DCheckOpenGLStateFilterWrapper::checkState(const Z3DFilter *p)
{

  if (!checkGLState(GL_BLEND, false)) {
    glDisable(GL_BLEND);
    warn(p, "GL_BLEND was enabled");
  }

  if (!checkGLState(GL_BLEND_SRC, GL_ONE) || !checkGLState(GL_BLEND_DST, GL_ZERO)) {
    glBlendFunc(GL_ONE, GL_ZERO);
    warn(p, "Modified BlendFunc");
  }

  if (!checkGLState(GL_DEPTH_TEST, false)) {
    glDisable(GL_DEPTH_TEST);
    warn(p, "GL_DEPTH_TEST was enabled");
  }

  if (!checkGLState(GL_CULL_FACE, false)) {
    glDisable(GL_CULL_FACE);
    warn(p, "GL_CULL_FACE was enabled");
  }

  if (!checkGLState(GL_COLOR_CLEAR_VALUE, glm::vec4(0.f))) {
    glClearColor(0.f, 0.f, 0.f, 0.f);
    warn(p, "glClearColor() was not set to all zeroes");
  }

  if (!checkGLState(GL_DEPTH_CLEAR_VALUE, 1.f)) {
    glClearDepth(1.0);
    warn(p, "glClearDepth() was not set to 1.0");
  }

  if (!checkGLState(GL_LINE_WIDTH, 1.f)) {
    glLineWidth(1.f);
    warn(p, "glLineWidth() was not set to 1.0");
  }

  if (!checkGLState(GL_MATRIX_MODE, GL_MODELVIEW)) {
    glMatrixMode(GL_MODELVIEW);
    warn(p, "glMatrixMode was not set to GL_MODELVIEW");
  }

  if (!checkGLState(GL_ACTIVE_TEXTURE, GL_TEXTURE0)) {
    glActiveTexture(GL_TEXTURE0);
    warn(p, "glActiveTexture was not set to GL_TEXTURE0");
  }

  if (!checkGLState(GL_TEXTURE_1D, false)) {
    glDisable(GL_TEXTURE_1D);
    warn(p, "GL_TEXTURE_1D was enabled");
  }

  if (!checkGLState(GL_TEXTURE_2D, false)) {
    glDisable(GL_TEXTURE_2D);
    warn(p, "GL_TEXTURE_2D was enabled");
  }

  if (!checkGLState(GL_TEXTURE_3D, false)) {
    glDisable(GL_TEXTURE_3D);
    warn(p, "GL_TEXTURE_3D was enabled");
  }

  GLint id;
  glGetIntegerv(GL_CURRENT_PROGRAM, &id);
  if (id != 0) {
    glUseProgram(0);
    warn(p, "A shader was active");
  }

  // can not check this as we are drawing to QOpenglWidget's (Qt5) fbo which is not 0
  //  if (Z3DRenderTarget::currentBoundDrawFBO() != 0) {
  //    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  //    warn(p, "A render target was bound (releaseTarget() missing?)");
  //  }

  if (!checkGLState(GL_DEPTH_FUNC, GL_LESS)) {
    glDepthFunc(GL_LESS);
    warn(p, "glDepthFunc was not set to GL_LESS");
  }

  if (!checkGLState(GL_CULL_FACE_MODE, GL_BACK)) {
    glCullFace(GL_BACK);
    warn(p, "glCullFace was not set to GL_BACK");
  }
}

void Z3DCheckOpenGLStateFilterWrapper::warn(const Z3DFilter* p, const QString& message)
{
  if (p) {
    LOG(WARNING) << "Invalid OpenGL state after processing " << p->className() << " : " << message;
  }
  else {
    LOG(WARNING) << "Invalid OpenGL state before network processing: " << message;
  }
}


void Z3DProfileFilterWrapper::beforeFilterProcess(const Z3DFilter *)
{
  m_benchTimer.start();
}

void Z3DProfileFilterWrapper::afterFilterProcess(const Z3DFilter *p)
{
  m_benchTimer.stop();
  LOG(INFO) << "Filter " << p->className() << " took time: " << m_benchTimer.time() << " seconds.";
}

void Z3DProfileFilterWrapper::beforeNetworkProcess()
{
  m_benchTimer.reset();
}

void Z3DProfileFilterWrapper::afterNetworkProcess()
{
  LOG(INFO) << "Network took time: " << m_benchTimer.total() << " seconds.";
}

} // namespace nim
