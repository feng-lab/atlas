#include "z3dswcview.h"

#include "z3dcanvas.h"

namespace nim {

Z3DSwcView::Z3DSwcView(ZSwcDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZSwcDoc, Z3DSwcFilter>(doc, engine)
{
  docSwcsAdded(m_doc.objs());
  connect(&m_doc, &ZSwcDoc::objAdded, this, &Z3DSwcView::docSwcAdded);
}

void Z3DSwcView::docSwcsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DSwcFilter(m_engine.globalParas(), this);
      viewControl->setData(m_doc.swcPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(m_engine.compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DSwcFilter::boundBoxChanged, this, &Z3DSwcView::updateBoundBox);
      connect(viewControl, &Z3DSwcFilter::objDeselected, this, &Z3DSwcView::onObjDeselectedFromView);
      connect(viewControl, &Z3DSwcFilter::objSelected, this, &Z3DSwcView::onObjSelectedFromView);
      connect(viewControl, &Z3DSwcFilter::objVisibleChanged, this, &Z3DSwcView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DSwcFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
      m_engine.addEventListenerToBack(*viewControl);
    }
    if (!objs.empty()) {
      m_engine.networkEvaluator().updateNetwork();
      m_engine.updateBoundBox();

      for (auto id : objs) {
        Q_EMIT objViewReady(id);
      }
    }
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render swc: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DSwcView::docSwcAdded(size_t id)
{
  try {
    auto viewControl = new Z3DSwcFilter(m_engine.globalParas(), this);
    viewControl->setData(m_doc.swcPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(m_engine.compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DSwcFilter::boundBoxChanged, this, &Z3DSwcView::updateBoundBox);
    connect(viewControl, &Z3DSwcFilter::objDeselected, this, &Z3DSwcView::onObjDeselectedFromView);
    connect(viewControl, &Z3DSwcFilter::objSelected, this, &Z3DSwcView::onObjSelectedFromView);
    connect(viewControl, &Z3DSwcFilter::objVisibleChanged, this, &Z3DSwcView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DSwcFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.networkEvaluator().updateNetwork();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render swc: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
