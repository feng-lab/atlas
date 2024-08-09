#include "z3dnetworkevaluator.h"

#include "z3dgl.h"
#include "z3dcompositor.h"
#include "z3dfilter.h"
#include "z3dmeshfilter.h"
#include "zlog.h"
#include "zcancellation.h"
#include <boost/graph/topological_sort.hpp>
#include <algorithm>
#include <queue>

#define PROFILE3DRENDERERS
#define CHECKOPENGLSTATE

namespace nim {

Z3DNetworkEvaluator::Z3DNetworkEvaluator(Z3DCompositor& compositor, QObject* parent)
  : QObject(parent)
  , m_compositor(compositor)
{
#if defined(CHECKOPENGLSTATE) || defined(ATLAS_SANITIZE_ADDRESS)
  m_filterWrappers.emplace_back(std::make_unique<Z3DCheckOpenGLStateFilterWrapper>());
#endif
#if defined(PROFILE3DRENDERERS)
  m_filterWrappers.emplace_back(std::make_unique<Z3DProfileFilterWrapper>());
#endif

  updateNetwork();
}

double
Z3DNetworkEvaluator::process(bool stereo, bool progressiveRendering, const folly::CancellationToken& cancellationToken)
{
  //  if (m_locked) {
  //    VLOG(1) << "locked. Scheduling.";
  //    // m_processPending = true;
  //    return;
  //  }
  //
  //  m_locked = true;

  // already locked

  //  for (size_t i = 0; i < m_renderingOrder.size(); ++i) {
  //    Z3DMeshFilter* meshFilter = qobject_cast<Z3DMeshFilter*>(m_renderingOrder[i]);
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

  maybeCancel(cancellationToken);

  // notify filter wrappers
  for (auto& filterWrapper : m_filterWrappers) {
    filterWrapper->beforeNetworkProcess();
  }
  CHECK_GL_ERROR

  double currentProgress = 0.0;
  double totalProgress = 0.0;

  // Iterate over filters in rendering order
  for (auto currentFilter : m_renderingOrder) {
    maybeCancel(cancellationToken);

    currentFilter->setProgressiveRenderingMode(progressiveRendering);

    Z3DEye eye = stereo ? LeftEye : MonoEye;

    // execute the filter, if it needs processing and is ready
    if (!currentFilter->isValid(eye) && currentFilter->isReady(eye)) {
      // notify filter wrappers
      for (auto& filterWrapper : m_filterWrappers) {
        filterWrapper->beforeFilterProcess(currentFilter);
      }
      CHECK_GL_ERROR

      {
        double progress = currentFilter->process(eye);
        // currentFilter->setValid(eye);
        if (progress == 1.0) {
          if (currentFilter == &m_compositor) {
            if (totalProgress == currentProgress) {
              currentFilter->setValid(eye);
            }
          } else {
            currentFilter->setValid(eye);
          }
        }
        currentProgress += progress;
        totalProgress += 1.0;
        CHECK_GL_ERROR
      }

      // notify filter wrappers
      for (const auto& filterWrapper : m_filterWrappers) {
        filterWrapper->afterFilterProcess(currentFilter);
      }
      CHECK_GL_ERROR
    }

    if (stereo && !currentFilter->isValid(RightEye) && currentFilter->isReady(RightEye)) {
      // notify filter wrappers
      for (const auto& filterWrapper : m_filterWrappers) {
        filterWrapper->beforeFilterProcess(currentFilter);
      }
      CHECK_GL_ERROR

      {
        double progress = currentFilter->process(RightEye);
        // currentFilter->setValid(Right);
        if (progress == 1.0) {
          if (currentFilter == &m_compositor) {
            if (totalProgress == currentProgress) {
              currentFilter->setValid(RightEye);
            }
          } else {
            currentFilter->setValid(RightEye);
          }
        }
        currentProgress += progress;
        totalProgress += 1.0;
        CHECK_GL_ERROR
      }

      // notify filter wrappers
      for (const auto& filterWrapper : m_filterWrappers) {
        filterWrapper->afterFilterProcess(currentFilter);
      }
      CHECK_GL_ERROR
    }
  }

  // notify filter wrappers
  for (const auto& filterWrapper : m_filterWrappers) {
    filterWrapper->afterNetworkProcess();
  }
  CHECK_GL_ERROR

  // m_locked = false;

  //  // make sure that canvases are repainted, if their update has been blocked by the locked evaluator
  //  if (m_processPending) {
  //    m_processPending = false;
  //    m_compositor.invalidate(Z3DFilter::State::AllResultInvalid);
  //  }

  if (!progressiveRendering) {
    CHECK(currentProgress == totalProgress) << currentProgress << " " << totalProgress;
  }
  return totalProgress > 0 ? currentProgress / totalProgress : 1.0;
}

void Z3DNetworkEvaluator::updateNetwork()
{
  m_renderingOrder.clear();
  m_filterToVertexMapper.clear();
  m_filterGraph.clear();
  m_reverseSortedFilters.clear();

  std::queue<Z3DFilter*> filterQueue;

  filterQueue.push(&m_compositor);
  Vertex v = boost::add_vertex(VertexInfo(&m_compositor), m_filterGraph);
  m_filterToVertexMapper[&m_compositor] = v;

  // build graph of all connected filters
  while (!filterQueue.empty()) {
    Z3DFilter* filter = filterQueue.front();
    const std::vector<Z3DInputPortBase*>& inports = filter->inputPorts();
    for (auto inport : inports) {
      const std::vector<Z3DOutputPortBase*> connected = inport->connected();
      for (auto j : connected) {
        Z3DFilter* outFilter = j->filter();
        if (!m_filterToVertexMapper.contains(outFilter)) {
          filterQueue.push(outFilter);
          v = boost::add_vertex(VertexInfo(outFilter), m_filterGraph);
          m_filterToVertexMapper[outFilter] = v;
        }
        boost::add_edge(m_filterToVertexMapper[outFilter],
                        m_filterToVertexMapper[filter],
                        EdgeInfo(j, inport),
                        m_filterGraph);
      }
    }

    filterQueue.pop();
  }

  // sort to get rendering order
  std::vector<Vertex> sorted;
  boost::topological_sort(m_filterGraph, std::back_inserter(sorted));
  for (auto rv : makeReverse(sorted)) {
    m_renderingOrder.push_back(m_filterGraph[rv].filter);
  }

  LOG(INFO) << "Rendering Order: ";
  for (size_t i = 0; i < m_renderingOrder.size(); ++i) {
    LOG(INFO) << "  " << i << ": " << m_renderingOrder[i]->className();
  }
  LOG(INFO) << "";

  // update reverse sorted filters
  m_reverseSortedFilters = m_renderingOrder;
  std::reverse(m_reverseSortedFilters.begin(), m_reverseSortedFilters.end());

  // update size
  sizeChangedFromFilter();
  for (auto filter : m_reverseSortedFilters) {
    QObject::disconnect(filter, &Z3DFilter::requestUpstreamSizeChange, nullptr, nullptr);
    connect(filter, &Z3DFilter::requestUpstreamSizeChange, this, &Z3DNetworkEvaluator::sizeChangedFromFilter);
  }
}

void Z3DNetworkEvaluator::sizeChangedFromFilter(Z3DFilter* rp)
{
  if (rp) {
    bool started = false;
    for (auto filter : m_reverseSortedFilters) {
      if (started) {
        filter->updateSize();
      } else {
        if (rp == filter) {
          started = true;
        }
      }
    }
  } else {
    for (auto filter : m_reverseSortedFilters) {
      filter->updateSize();
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

void Z3DCheckOpenGLStateFilterWrapper::checkState(const Z3DFilter* p)
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

  if (!checkGLState(GL_ACTIVE_TEXTURE, GL_TEXTURE0)) {
    glActiveTexture(GL_TEXTURE0);
    warn(p, "glActiveTexture was not set to GL_TEXTURE0");
  }

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  if (!checkGLState(GL_MATRIX_MODE, GL_MODELVIEW)) {
    glMatrixMode(GL_MODELVIEW);
    warn(p, "glMatrixMode was not set to GL_MODELVIEW");
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
#endif

  GLint id;
  glGetIntegerv(GL_CURRENT_PROGRAM, &id);
  if (id != 0) {
    glUseProgram(0);
    warn(p, "A shader was active");
  }

  // can not check this as we are drawing to QOpenglWidget's (Qt5) fbo which is not 0
#if 0
  if (Z3DRenderTarget::currentBoundDrawFBO() != 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    warn(p, "A render target was bound (releaseTarget() missing?)");
  }
#endif

  if (!checkGLState(GL_DEPTH_FUNC, GL_LESS)) {
    glDepthFunc(GL_LESS);
    warn(p, "glDepthFunc was not set to GL_LESS");
  }

  if (!checkGLState(GL_CULL_FACE_MODE, GL_BACK)) {
    glCullFace(GL_BACK);
    warn(p, "glCullFace was not set to GL_BACK");
  }
}

void Z3DCheckOpenGLStateFilterWrapper::warn(const Z3DFilter* p, const char* message)
{
  if (p) {
    LOG(WARNING) << "Invalid OpenGL state after processing " << p->className() << " : " << message;
  } else {
    LOG(WARNING) << "Invalid OpenGL state before network processing: " << message;
  }
}

void Z3DProfileFilterWrapper::afterFilterProcess(const Z3DFilter* p)
{
  m_benchTimer.recordEvent(p->className().toStdString());
}

void Z3DProfileFilterWrapper::beforeNetworkProcess()
{
  m_benchTimer.resetAndStart("Network");
}

void Z3DProfileFilterWrapper::afterNetworkProcess()
{
  m_benchTimer.stop();
  LOG(INFO) << m_benchTimer.toString();
}

} // namespace nim
