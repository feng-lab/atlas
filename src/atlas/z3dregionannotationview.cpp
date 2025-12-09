#include "z3dregionannotationview.h"

namespace nim {

Z3DRegionAnnotationView::Z3DRegionAnnotationView(ZRegionAnnotationDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZRegionAnnotationDoc, Z3DRegionAnnotationFilter>(doc, engine)
{
  docRegionAnnotationsAdded(m_doc.objs());
  connect(&m_doc, &ZRegionAnnotationDoc::objAdded, this, &Z3DRegionAnnotationView::docRegionAnnotationAdded);
}

void Z3DRegionAnnotationView::docRegionAnnotationsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DRegionAnnotationFilter(m_engine.globalParas(), this);
      viewControl->setData(m_doc.regionAnnotationPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objDeselected,
              this,
              &Z3DRegionAnnotationView::onObjDeselectedFromView);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objSelected,
              this,
              &Z3DRegionAnnotationView::onObjSelectedFromView);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objVisibleChanged,
              this,
              &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DRegionAnnotationFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
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
    auto errorMsg = fmt::format("Failed to render regionAnnotation: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  try {
    auto viewControl = new Z3DRegionAnnotationFilter(m_engine.globalParas(), this);
    viewControl->setData(m_doc.regionAnnotationPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objDeselected,
            this,
            &Z3DRegionAnnotationView::onObjDeselectedFromView);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objSelected,
            this,
            &Z3DRegionAnnotationView::onObjSelectedFromView);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objVisibleChanged,
            this,
            &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DRegionAnnotationFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render regionAnnotation: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
