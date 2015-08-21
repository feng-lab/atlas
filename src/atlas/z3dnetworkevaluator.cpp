#include "z3dgl.h"
#include "z3dnetworkevaluator.h"
#include <set>
#include <queue>
#include <algorithm>
#include <boost/graph/topological_sort.hpp>
#include "z3dcanvaspainter.h"
#include "z3dprocessor.h"
#include "QsLog.h"
#include "z3dtexture.h"
#include "z3drendertarget.h"

//#define PROFILE3DRENDERERS

namespace nim {

Z3DNetworkEvaluator::Z3DNetworkEvaluator(QObject *parent)
  : QObject(parent)
  , m_renderingOrder()
  , m_processWrappers()
  , m_openGLContext(NULL)
  , m_locked(false)
  , m_processPending(false)
  , m_canvasPainter(NULL)
{
#if defined(_DEBUG_)
  addProcessWrapper(new Z3DCheckOpenGLStateProcessWrapper());
#endif
#if defined(PROFILE3DRENDERERS)
  addProcessWrapper(new Z3DProfileProcessWrapper());
#endif
}

Z3DNetworkEvaluator::~Z3DNetworkEvaluator()
{
  clearProcessWrappers();
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
    LDEBUG() << "locked. Scheduling.";
    m_processPending = true;
    return;
  }

  lock();

  // notify process wrappers
  for (size_t j = 0; j < m_processWrappers.size(); ++j)
    m_processWrappers[j]->beforeNetworkProcess();
  CHECK_GL_ERROR;

  // Iterate over processing in rendering order
  for (size_t i = 0; i < m_renderingOrder.size(); ++i) {
    Z3DProcessor* currentProcessor = m_renderingOrder[i];

    Z3DEye eye = stereo ? Z3DEye::Left : Z3DEye::Mono;

    // run the processor, if it needs processing and is ready
    if (!currentProcessor->isValid(eye) && currentProcessor->isReady(eye)) {
      // notify process wrappers
      for (size_t j=0; j < m_processWrappers.size(); ++j)
        m_processWrappers[j]->beforeProcess(currentProcessor);
      CHECK_GL_ERROR;

      {
        getGLFocus();
        currentProcessor->process(eye);
        currentProcessor->setValid(eye);
        CHECK_GL_ERROR;
      }

      // notify process wrappers
      getGLFocus();
      for (size_t j = 0; j < m_processWrappers.size(); ++j)
        m_processWrappers[j]->afterProcess(currentProcessor);
      CHECK_GL_ERROR;
    }

    if (stereo && !currentProcessor->isValid(Z3DEye::Right) && currentProcessor->isReady(Z3DEye::Right)) {
      // notify process wrappers
      for (size_t j=0; j < m_processWrappers.size(); ++j)
        m_processWrappers[j]->beforeProcess(currentProcessor);
      CHECK_GL_ERROR;

      {
        getGLFocus();
        currentProcessor->process(Z3DEye::Right);
        currentProcessor->setValid(Z3DEye::Right);
        CHECK_GL_ERROR;
      }

      // notify process wrappers
      getGLFocus();
      for (size_t j = 0; j < m_processWrappers.size(); ++j)
        m_processWrappers[j]->afterProcess(currentProcessor);
      CHECK_GL_ERROR;
    }
  }

  // notify process wrappers
  for (size_t j = 0; j < m_processWrappers.size(); ++j)
    m_processWrappers[j]->afterNetworkProcess();
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
    LDEBUG() << "locked.";
  }

  lock();

  // update size
  sizeChangedFromProcessor();
  for (size_t i=0; i<m_reverseSortedRenderProcessors.size(); i++) {
    QObject::disconnect(m_reverseSortedRenderProcessors[i],
                        SIGNAL(requestUpstreamSizeChange(Z3DProcessor*)),
                        0, 0);
    connect(m_reverseSortedRenderProcessors[i], SIGNAL(requestUpstreamSizeChange(Z3DProcessor*)),
            this, SLOT(sizeChangedFromProcessor(Z3DProcessor*)));
  }

  unlock();
  CHECK_GL_ERROR;
}

void Z3DNetworkEvaluator::addProcessWrapper(Z3DProcessWrapper* w)
{
  m_processWrappers.push_back(w);
}

void Z3DNetworkEvaluator::removeProcessWrapper(const Z3DProcessWrapper* w)
{
  std::vector<Z3DProcessWrapper*>::iterator it = std::find(m_processWrappers.begin(), m_processWrappers.end(), w);
  if (it != m_processWrappers.end()) {
    m_processWrappers.erase(it);
    delete w;
  }
}

void Z3DNetworkEvaluator::clearProcessWrappers()
{
  for (size_t i=0; i<m_processWrappers.size(); ++i) {
    delete m_processWrappers[i];
  }

  m_processWrappers.clear();
}

void Z3DNetworkEvaluator::updateNetwork()
{
  buildNetwork();
  initializeNetwork();
}

void Z3DNetworkEvaluator::buildNetwork()
{
  m_renderingOrder.clear();
  m_processorToVertexMapper.clear();
  m_processorGraph.clear();
  m_reverseSortedRenderProcessors.clear();

  // nothing more to do, if no network sink is present
  if (!m_canvasPainter)
    return;

  std::set<Z3DProcessor*> processed;
  std::queue<Z3DProcessor*> processQueue;

  processQueue.push(m_canvasPainter);
  Vertex v = boost::add_vertex(VertexInfo(m_canvasPainter), m_processorGraph);
  m_processorToVertexMapper[m_canvasPainter] = v;

  // build graph of all connected processors
  while (!processQueue.empty()) {
    Z3DProcessor *processor = processQueue.front();
    const std::vector<Z3DInputPortBase*> inports = processor->inputPorts();
    for (size_t i = 0; i < inports.size(); ++i) {
      const std::vector<Z3DOutputPortBase*> connected = inports[i]->connected();
      for (size_t j = 0; j < connected.size(); ++j) {
        Z3DProcessor *outProcessor = connected[j]->processor();
        if (m_processorToVertexMapper.find(outProcessor) == m_processorToVertexMapper.end()) {
          processQueue.push(outProcessor);
          Vertex v = boost::add_vertex(VertexInfo(outProcessor), m_processorGraph);
          m_processorToVertexMapper[outProcessor] = v;
        }
        boost::add_edge(m_processorToVertexMapper[outProcessor],
                        m_processorToVertexMapper[processor],
                        EdgeInfo(connected[j], inports[i]),
                        m_processorGraph);
      }
    }

    processed.insert(processor);
    processQueue.pop();
  }

  // sort to get rendering order
  std::vector<Vertex> sorted;
  boost::topological_sort(m_processorGraph, std::back_inserter(sorted));
  for (std::vector<Vertex>::reverse_iterator rit = sorted.rbegin();
       rit != sorted.rend(); rit++) {
    m_renderingOrder.push_back(m_processorGraph[*rit].processor);
  }

  LINFO() << "Rendering Order: ";
  for (size_t i=0; i<m_renderingOrder.size(); i++) {
    LINFO() << "  " << i << ": " << m_renderingOrder[i]->className();
  }
  LINFO() << "";

  // update reverse sorted renderprocessors
  m_reverseSortedRenderProcessors = m_renderingOrder;
  std::reverse(m_reverseSortedRenderProcessors.begin(), m_reverseSortedRenderProcessors.end());
}

void Z3DNetworkEvaluator::sizeChangedFromProcessor(Z3DProcessor *rp)
{
  if (rp) {
    bool started = false;
    for (size_t i=0; i<m_reverseSortedRenderProcessors.size(); i++) {
      if (started)
        m_reverseSortedRenderProcessors[i]->updateSize();
      else {
        if (rp == m_reverseSortedRenderProcessors[i])
          started = true;
      }
    }
  } else {
    for (size_t i=0; i<m_reverseSortedRenderProcessors.size(); i++) {
      m_reverseSortedRenderProcessors[i]->updateSize();
    }
  }
}

// ----------------------------------------------------------------------------

void Z3DCheckOpenGLStateProcessWrapper::afterProcess(const Z3DProcessor* p)
{
  checkState(p);
}

void Z3DCheckOpenGLStateProcessWrapper::beforeNetworkProcess()
{
  checkState();
}

void Z3DCheckOpenGLStateProcessWrapper::checkState(const Z3DProcessor *p)
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

  if (Z3DRenderTarget::currentBoundDrawFBO() != 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    warn(p, "A render target was bound (releaseTarget() missing?)");
  }

  if (!checkGLState(GL_DEPTH_FUNC, GL_LESS)) {
    glDepthFunc(GL_LESS);
    warn(p, "glDepthFunc was not set to GL_LESS");
  }

  if (!checkGLState(GL_CULL_FACE_MODE, GL_BACK)) {
    glCullFace(GL_BACK);
    warn(p, "glCullFace was not set to GL_BACK");
  }
}

void Z3DCheckOpenGLStateProcessWrapper::warn(const Z3DProcessor* p, const QString& message)
{
  if (p) {
    LWARN() << "Invalid OpenGL state after processing" << p->className() << ":" << message;
  }
  else {
    LWARN() << "Invalid OpenGL state before network processing:" << message;
  }
}


void Z3DProfileProcessWrapper::beforeProcess(const Z3DProcessor *)
{
  m_benchTimer.start();
}

void Z3DProfileProcessWrapper::afterProcess(const Z3DProcessor *p)
{
  m_benchTimer.stop();
  LINFO() << "Process" << p->className() << "took time:" << m_benchTimer.time() << "seconds.";
}

void Z3DProfileProcessWrapper::beforeNetworkProcess()
{
  m_benchTimer.reset();
}

void Z3DProfileProcessWrapper::afterNetworkProcess()
{
  LINFO() << "Network took time:" << m_benchTimer.total() << "seconds.";
}

} // namespace nim
