#include "z3dskeletonview.h"

namespace nim {

Z3DSkeletonView::Z3DSkeletonView(ZSkeletonDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZSkeletonDoc, Z3DSkeletonFilter>(doc, engine)
{
  docSkeletonsAdded(m_doc.objs());
  connect(&m_doc, &ZSkeletonDoc::objAdded, this, &Z3DSkeletonView::docSkeletonAdded);
  connect(&m_doc, &ZSkeletonDoc::skeletonChanged, this, [this](size_t id) {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end()) {
      return;
    }
    it->second->setData(m_doc.skeleton(id));
  });
}

void Z3DSkeletonView::docSkeletonsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DSkeletonFilter(m_engine.globalParas(), this);
      viewControl->setData(m_doc.skeleton(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DSkeletonFilter::boundBoxChanged, this, &Z3DSkeletonView::updateBoundBox);
      connect(viewControl, &Z3DSkeletonFilter::objDeselected, this, &Z3DSkeletonView::onObjDeselectedFromView);
      connect(viewControl, &Z3DSkeletonFilter::objSelected, this, &Z3DSkeletonView::onObjSelectedFromView);
      connect(viewControl, &Z3DSkeletonFilter::objVisibleChanged, this, &Z3DSkeletonView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DSkeletonFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
      m_engine.addEventListenerToBack(*viewControl);
    }
    if (!objs.empty()) {
      m_engine.updatePipeline();
      m_engine.updateBoundBox();

      for (auto id : objs) {
        Q_EMIT objViewReady(id);
      }
    }
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render skeleton: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DSkeletonView::docSkeletonAdded(size_t id)
{
  try {
    auto viewControl = new Z3DSkeletonFilter(m_engine.globalParas(), this);
    viewControl->setData(m_doc.skeleton(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DSkeletonFilter::boundBoxChanged, this, &Z3DSkeletonView::updateBoundBox);
    connect(viewControl, &Z3DSkeletonFilter::objDeselected, this, &Z3DSkeletonView::onObjDeselectedFromView);
    connect(viewControl, &Z3DSkeletonFilter::objSelected, this, &Z3DSkeletonView::onObjSelectedFromView);
    connect(viewControl, &Z3DSkeletonFilter::objVisibleChanged, this, &Z3DSkeletonView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DSkeletonFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render skeleton: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim

