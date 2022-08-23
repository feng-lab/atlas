#include "z3dpunctaview.h"

namespace nim {

Z3DPunctaView::Z3DPunctaView(ZPunctaDoc& doc, Z3DView& view)
  : Z3DFilterView<ZPunctaDoc, Z3DPunctaFilter>(doc, view)
{
  docPunctasAdded(m_doc.objs());
  connect(&m_doc, &ZPunctaDoc::objAdded, this, &Z3DPunctaView::docPunctaAdded);
}

void Z3DPunctaView::docPunctasAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DPunctaFilter(globalParas(), this);
      viewControl->setData(m_doc.punctaPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DPunctaFilter::boundBoxChanged, this, &Z3DPunctaView::updateBoundBox);
      connect(viewControl, &Z3DPunctaFilter::objDeselected, this, &Z3DPunctaView::onObjDeselectedFromView);
      connect(viewControl, &Z3DPunctaFilter::objSelected, this, &Z3DPunctaView::onObjSelectedFromView);
      connect(viewControl, &Z3DPunctaFilter::objVisibleChanged, this, &Z3DPunctaView::onObjVisibleChangedFromView);
      canvas().addEventListenerToBack(*viewControl);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();

      for (auto id : objs) {
        Q_EMIT objViewReady(id);
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render puncta: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render puncta:\n%1").arg(e.what()));
  }
}

void Z3DPunctaView::docPunctaAdded(size_t id)
{
  try {
    auto viewControl = new Z3DPunctaFilter(globalParas(), this);
    viewControl->setData(m_doc.punctaPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DPunctaFilter::boundBoxChanged, this, &Z3DPunctaView::updateBoundBox);
    connect(viewControl, &Z3DPunctaFilter::objDeselected, this, &Z3DPunctaView::onObjDeselectedFromView);
    connect(viewControl, &Z3DPunctaFilter::objSelected, this, &Z3DPunctaView::onObjSelectedFromView);
    connect(viewControl, &Z3DPunctaFilter::objVisibleChanged, this, &Z3DPunctaView::onObjVisibleChangedFromView);
    canvas().addEventListenerToBack(*viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render puncta: " << e.what();
    QMessageBox::critical(&m_view.canvas(),
                          QApplication::applicationName(),
                          QString("Failed to render puncta:\n%1").arg(e.what()));
  }
}

} // namespace nim
