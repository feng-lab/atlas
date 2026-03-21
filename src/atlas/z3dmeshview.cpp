#include "z3dmeshview.h"

namespace nim {

Z3DMeshView::Z3DMeshView(ZMeshDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZMeshDoc, Z3DMeshFilter>(doc, engine)
{
  docMeshesAdded(m_doc.objs());
  connect(&m_doc, &ZMeshDoc::objAdded, this, &Z3DMeshView::docMeshAdded);
  connect(&m_doc, &ZMeshDoc::meshChanged, this, [this](size_t id) {
    auto it = m_idToFilter.find(id);
    if (it == m_idToFilter.end()) {
      return;
    }
    it->second->setData(m_doc.meshList(id));
    it->second->setExternalSourceState(m_doc.jsonValue(id), m_doc.externalRemoteContext(id));
  });
}

void Z3DMeshView::docMeshesAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DMeshFilter(m_engine.globalParas(), nullptr, this);
      viewControl->setData(m_doc.meshList(id));
      viewControl->setExternalSourceState(m_doc.jsonValue(id), m_doc.externalRemoteContext(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DMeshFilter::boundBoxChanged, this, &Z3DMeshView::updateBoundBox);
      connect(viewControl, &Z3DMeshFilter::objDeselected, this, &Z3DMeshView::onObjDeselectedFromView);
      connect(viewControl, &Z3DMeshFilter::objSelected, this, &Z3DMeshView::onObjSelectedFromView);
      connect(viewControl, &Z3DMeshFilter::objVisibleChanged, this, &Z3DMeshView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DMeshFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
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
    auto errorMsg = fmt::format("Failed to render mesh: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DMeshView::docMeshAdded(size_t id)
{
  try {
    auto viewControl = new Z3DMeshFilter(m_engine.globalParas(), nullptr, this);
    viewControl->setData(m_doc.meshList(id));
    viewControl->setExternalSourceState(m_doc.jsonValue(id), m_doc.externalRemoteContext(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DMeshFilter::boundBoxChanged, this, &Z3DMeshView::updateBoundBox);
    connect(viewControl, &Z3DMeshFilter::objDeselected, this, &Z3DMeshView::onObjDeselectedFromView);
    connect(viewControl, &Z3DMeshFilter::objSelected, this, &Z3DMeshView::onObjSelectedFromView);
    connect(viewControl, &Z3DMeshFilter::objVisibleChanged, this, &Z3DMeshView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DMeshFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render mesh: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
