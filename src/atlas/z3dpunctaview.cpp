#include "z3dpunctaview.h"

namespace nim {

Z3DPunctaView::Z3DPunctaView(ZPunctaDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZPunctaDoc, Z3DPunctaFilter>(doc, engine)
{
  docPunctasAdded(m_doc.objs());
  connect(&m_doc, &ZPunctaDoc::objAdded, this, &Z3DPunctaView::docPunctaAdded);
}

void Z3DPunctaView::docPunctasAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DPunctaFilter(m_engine.globalParas(), this);
      viewControl->setData(m_doc.punctaPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DPunctaFilter::boundBoxChanged, this, &Z3DPunctaView::updateBoundBox);
      connect(viewControl, &Z3DPunctaFilter::objDeselected, this, &Z3DPunctaView::onObjDeselectedFromView);
      connect(viewControl, &Z3DPunctaFilter::objSelected, this, &Z3DPunctaView::onObjSelectedFromView);
      connect(viewControl, &Z3DPunctaFilter::objVisibleChanged, this, &Z3DPunctaView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DPunctaFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
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
    auto errorMsg = fmt::format("Failed to render puncta: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DPunctaView::docPunctaAdded(size_t id)
{
  try {
    auto viewControl = new Z3DPunctaFilter(m_engine.globalParas(), this);
    viewControl->setData(m_doc.punctaPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DPunctaFilter::boundBoxChanged, this, &Z3DPunctaView::updateBoundBox);
    connect(viewControl, &Z3DPunctaFilter::objDeselected, this, &Z3DPunctaView::onObjDeselectedFromView);
    connect(viewControl, &Z3DPunctaFilter::objSelected, this, &Z3DPunctaView::onObjSelectedFromView);
    connect(viewControl, &Z3DPunctaFilter::objVisibleChanged, this, &Z3DPunctaView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DPunctaFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render puncta: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
