#include "z3danimationview.h"

#include "z3dcanvas.h"

namespace nim {

Z3DAnimationView::Z3DAnimationView(Z3DAnimationDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<Z3DAnimationDoc, Z3DAnimationFilter>(doc, engine)
{
  docAnimationsAdded(m_doc.objs());
  connect(&m_doc, &Z3DAnimationDoc::objAdded, this, &Z3DAnimationView::docAnimationAdded);
}

void Z3DAnimationView::docAnimationsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DAnimationFilter(m_engine.globalParas(), this);
      viewControl->setData(&m_doc.animation(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      // Sync initial visibility from the document state (object manager checkbox). This is important when
      // the 3D window is reopened while the document retains object visibility state.
      viewControl->setVisible(m_doc.isObjVisible(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DAnimationFilter::boundBoxChanged, this, &Z3DAnimationView::updateBoundBox);
      connect(viewControl,
              &Z3DAnimationFilter::objVisibleChanged,
              this,
              &Z3DAnimationView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DAnimationFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
      Q_EMIT objViewReady(id);
    }
    if (!objs.empty()) {
      m_engine.updatePipeline();
      m_engine.updateBoundBox();
    }
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render 3d animation: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DAnimationView::docAnimationAdded(size_t id)
{
  try {
    auto viewControl = new Z3DAnimationFilter(m_engine.globalParas(), this);
    viewControl->setData(&m_doc.animation(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    viewControl->setVisible(m_doc.isObjVisible(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DAnimationFilter::boundBoxChanged, this, &Z3DAnimationView::updateBoundBox);
    connect(viewControl, &Z3DAnimationFilter::objVisibleChanged, this, &Z3DAnimationView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DAnimationFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render 3d animation: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
