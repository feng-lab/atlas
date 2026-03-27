#include "z3dimgview.h"

namespace nim {

Z3DImgView::Z3DImgView(ZImgDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZImgDoc, Z3DImgFilter>(doc, engine)
{
  docImgsAdded(m_doc.objs());
  connect(&m_doc, &ZImgDoc::objAdded, this, &Z3DImgView::docImgAdded);
}

void Z3DImgView::docImgsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DImgFilter(m_engine.globalParas(), this);
      viewControl->setImgObjId(id);
      viewControl->setSeedTraceUiState(m_engine.seedTraceToolEnabled(),
                                       m_engine.seedTraceInProgress(),
                                       m_engine.seedTraceSourceImgObjId(),
                                       m_engine.seedTraceSourceChannel());
      viewControl->setData(m_doc.imgPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
      connect(viewControl, &Z3DImgFilter::boundBoxChanged, this, &Z3DImgView::updateBoundBox);
      connect(viewControl, &Z3DImgFilter::objDeselected, this, &Z3DImgView::onObjDeselectedFromView);
      connect(viewControl, &Z3DImgFilter::objSelected, this, &Z3DImgView::onObjSelectedFromView);
      connect(viewControl, &Z3DImgFilter::objVisibleChanged, this, &Z3DImgView::onObjVisibleChangedFromView);
      connect(viewControl, &Z3DImgFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
      connect(viewControl,
              &Z3DImgFilter::deferredRenderingWarning,
              &m_engine,
              &Z3DRenderingEngine::appendDeferredRenderingError);
      connect(viewControl,
              &Z3DImgFilter::showSeedTraceContextMenu,
              &m_engine,
              &Z3DRenderingEngine::showSeedTraceContextMenu);
      connect(viewControl,
              &Z3DImgFilter::pointInVolumeLeftClicked,
              &m_engine,
              &Z3DRenderingEngine::pointInVolumeLeftClicked);
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
    auto errorMsg = fmt::format("Failed to render image: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

void Z3DImgView::docImgAdded(size_t id)
{
  try {
    auto viewControl = new Z3DImgFilter(m_engine.globalParas(), this);
    viewControl->setImgObjId(id);
    viewControl->setSeedTraceUiState(m_engine.seedTraceToolEnabled(),
                                     m_engine.seedTraceInProgress(),
                                     m_engine.seedTraceSourceImgObjId(),
                                     m_engine.seedTraceSourceChannel());
    viewControl->setData(m_doc.imgPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    connect(viewControl, &Z3DFilter::invalidated, &m_engine.compositor(), &Z3DCompositor::invalidateResult);
    connect(viewControl, &Z3DImgFilter::boundBoxChanged, this, &Z3DImgView::updateBoundBox);
    connect(viewControl, &Z3DImgFilter::objDeselected, this, &Z3DImgView::onObjDeselectedFromView);
    connect(viewControl, &Z3DImgFilter::objSelected, this, &Z3DImgView::onObjSelectedFromView);
    connect(viewControl, &Z3DImgFilter::objVisibleChanged, this, &Z3DImgView::onObjVisibleChangedFromView);
    connect(viewControl, &Z3DImgFilter::renderingError, &m_engine, &Z3DRenderingEngine::renderingError);
    connect(viewControl,
            &Z3DImgFilter::deferredRenderingWarning,
            &m_engine,
            &Z3DRenderingEngine::appendDeferredRenderingError);
    connect(viewControl,
            &Z3DImgFilter::showSeedTraceContextMenu,
            &m_engine,
            &Z3DRenderingEngine::showSeedTraceContextMenu);
    connect(viewControl,
            &Z3DImgFilter::pointInVolumeLeftClicked,
            &m_engine,
            &Z3DRenderingEngine::pointInVolumeLeftClicked);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.updatePipeline();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    auto errorMsg = fmt::format("Failed to render image: {}", e.what());
    LOG(ERROR) << errorMsg;
    m_engine.reportRenderingError(errorMsg);
  }
}

} // namespace nim
